/*
 * hid.c -- raw HID input reports from a headset's inline remote.
 *
 * Ported from m5tab5_esp_idf_usb_host_example's usb_host_example.c, which
 * claims HID interfaces and dumps their reports as hex so the buttons can
 * be identified by pressing them. The claiming and the transfer lifecycle
 * are kept unchanged, because both were arrived at by hitting the failure
 * they avoid. The hex dump is kept too -- see hid_report() -- because it
 * is the only way to check the mapping below is right.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "usb/usb_host.h"

#include "hid.h"
#include "usbhost.h"

static const char *TAG = "tab5_hid";

#define HID_MAX_IN_REPORT       (16)

static usb_host_client_handle_t s_client;
static hid_button_cb_t          s_cb;

/*
 * Which bit of report byte 0 is which button.
 *
 * The bit numbers started as an inference -- the device stalls EP0 for
 * its report descriptor, so there was nothing to derive them from -- and
 * the logging added alongside them has confirmed the mapping:
 *
 *   remote: 02 00 00 00 -> +Vol-      press
 *   remote: 00 00 00 00 -> -Vol-      release
 *   remote: 04 00 00 00 -> +Mute      line-out mute
 *   remote: 08 00 00 00 -> +MicMute   line-in mute, a different key
 *
 * Bit 2 is the line-out mute and bit 3 is the line-in mute. Two separate
 * buttons: a session that presses them in turn produces a report
 * alternating between 0x04 and 0x08, which is a fact about how they were
 * pressed and not about the device.
 *
 * The bug that made mute appear to latch on was in the dispatch, not in
 * this table. HID_BTN_MIC_MUTE is dropped by player.c -- correctly, there
 * being no microphone in this program -- so every press of the line-in
 * key left no trace at all, and alternating the two keys looked like a
 * mute button that had stopped responding.
 *
 * If a button does the wrong thing, this is still a table edit, and the
 * hex above is still logged unconditionally to make it one.
 */
static const struct {
    uint8_t      bit;
    hid_button_t button;
    const char  *name;
} BUTTON_BITS[] = {
    { 0, HID_BTN_VOL_UP,   "Vol+"    },
    { 1, HID_BTN_VOL_DOWN, "Vol-"    },
    { 2, HID_BTN_MUTE,     "Mute"    },   /* line out: toggles the player */
    { 3, HID_BTN_MIC_MUTE, "MicMute" },   /* line in: logged, no action */
};

/*
 * How a given interface's reports are to be read.
 *
 * HID says a report means whatever its report descriptor says it means,
 * and until 0506 this file did not read one -- it treated byte 0 of
 * every report as a bitmask, because that is what the one device it was
 * written against sends. A boot keyboard's byte 0 is the modifier byte,
 * so plugging one in made every report look like a volume-up: the log
 * shows the volume walking 26% to 100% while F1..F12 were being pressed,
 * and byte 0 was Left Ctrl throughout.
 *
 * So the interface is classified once, at claim time, and the report is
 * read accordingly.
 *
 *   HID_KIND_BOOT_KEYBOARD  bInterfaceProtocol is 1. Byte 0 is
 *                           modifiers, byte 1 is reserved, bytes 2..7
 *                           are keycodes. HID 1.11, Appendix B.
 *
 *   HID_KIND_CONSUMER       the report descriptor declares Usage Page
 *                           (Consumer). Reports carry usage codes, and
 *                           the media keys live here -- 0xB5 next, 0xB6
 *                           previous, 0xCD play/pause, HID Usage Tables
 *                           chapter 15.
 *
 *   HID_KIND_BITMASK        the fallback, and the reason this is a
 *                           classification rather than a rewrite. The
 *                           headset remote this file was built for
 *                           STALLS EP0 for its report descriptor, so
 *                           there is nothing to parse and nothing to
 *                           derive. A device that will not describe
 *                           itself gets the old table and the old
 *                           behaviour, which is confirmed on hardware.
 */
typedef enum {
    HID_KIND_BITMASK = 0,
    HID_KIND_BOOT_KEYBOARD,
    HID_KIND_CONSUMER,
} hid_kind_t;

typedef struct {
    usb_device_handle_t dev;
    usb_transfer_t     *transfer;
    SemaphoreHandle_t   done;
    uint8_t itf_num;
    hid_kind_t kind;
    bool       report_ids;   /* descriptor declared Report IDs */
    uint8_t    keys[6];      /* boot keyboard: the previous keycodes */
} hid_ctx_t;

/*
 * Consumer usages this program can act on. HID Usage Tables 1.4,
 * chapter 15. Sixteen bits, because that is the width a Consumer usage
 * is reported in and truncating it to eight collides: 0xE9 is Volume
 * Increment and 0x00E9 is what a two-byte field carries.
 */
static const struct {
    uint16_t     usage;
    hid_button_t button;
    const char  *name;
} CONSUMER_USAGES[] = {
    { 0x00B5, HID_BTN_NEXT,       "Next"      },
    { 0x00B6, HID_BTN_PREV,       "Prev"      },
    { 0x00B7, HID_BTN_STOP,       "Stop"      },
    { 0x00CD, HID_BTN_PLAY_PAUSE, "Play"      },
    { 0x00E2, HID_BTN_MUTE,       "Mute"      },
    { 0x00E9, HID_BTN_VOL_UP,     "Vol+"      },
    { 0x00EA, HID_BTN_VOL_DOWN,   "Vol-"      },
};

/*
 * Boot-keyboard keycodes, for a keyboard with no consumer collection --
 * or one whose media keys are on an interface this did not get. HID
 * Usage Tables, keyboard page.
 *
 * Deliberately few. A keyboard is not a remote and guessing at a full
 * layout would be inventing a UI; these are the three keys every media
 * player already means these things by.
 */
static const struct {
    uint8_t      code;
    hid_button_t button;
    const char  *name;
} KEYBOARD_KEYS[] = {
    { 0x2C, HID_BTN_PLAY_PAUSE, "Space"  },
    { 0x4F, HID_BTN_NEXT,       "Right"  },
    { 0x50, HID_BTN_PREV,       "Left"   },
};

/*
 * Edge detect on byte 0, then dispatch.
 *
 * Presses only. These remotes re-send the same report while a button is
 * held and send a zero byte on release, so acting on the level rather
 * than the edge would fire volume-up forty times a second and toggle
 * mute back and forth for as long as a finger was on it.
 *
 * The zero-to-something transition is the press; everything else is a
 * repeat or a release and is ignored. Two buttons pressed together
 * arrive as two bits and both fire, which is the honest reading of the
 * report even though no remote here has two buttons close enough
 * together for it to happen by accident.
 */
static const int BUTTON_COUNT = (int)(sizeof(BUTTON_BITS) / sizeof(BUTTON_BITS[0]));

static int entry_for_bit(int bit)
{
    for (int i = 0; i < BUTTON_COUNT; i++) {
        if (BUTTON_BITS[i].bit == bit) return i;
    }
    return -1;
}

/*
 * One button, from a report that already is an event -- see hid_report().
 *
 * The timing heuristic that used to live here -- measure how long a bit
 * stays set, and read a slow release as a second press -- is gone. It
 * existed to tell a latching switch from a momentary button whose state
 * the software owns, and the mute state belongs to the player. There was
 * nothing to discriminate, and a threshold with nothing to discriminate
 * is only a way to get it wrong later.
 *
 * HID_BTN_MIC_MUTE is dispatched to player.c and dropped there rather
 * than filtered out here, so the press still appears in the log above. A
 * key that does nothing and says nothing is indistinguishable from one
 * whose reports are not arriving, which is exactly how the mute bug hid.
 */
static void dispatch_bit(hid_ctx_t *ctx, int bit)
{
    (void)ctx;
    const int e = entry_for_bit(bit);
    if (e >= 0 && s_cb) s_cb(BUTTON_BITS[e].button);
}

/*
 * Every report is a press. There is no edge detection here, and that is
 * the correction rather than an omission.
 *
 * The endpoint delivers a report when a key is pressed and not otherwise
 * -- it is silent between presses rather than repeating the current
 * state at the polling interval. What the earlier versions of this
 * function did was reconstruct button state from a stream that was
 * already a stream of events, and then suppress anything that did not
 * change that state. On this remote that discards real presses, because
 * the bits do not clear the way state bits would:
 *
 *   press Mute       04 00 00 00     dispatched
 *   press Mute       04 00 00 00     identical, suppressed  <- lost
 *   press MicMute    08 00 00 00     dispatched
 *   press Mute       04 00 00 00     changed, dispatched
 *
 * Which is exactly the reported symptom: mute only worked again after
 * mic-mute had been pressed, because mic-mute was the only thing that
 * made the next mute report differ from the last one. The volume keys
 * never showed it because they DO send a release -- 02 then 00 -- so
 * their next press always differed.
 *
 * So the state is gone and every report dispatches whatever bits it
 * carries. A report of 0x00 is a release and carries none.
 *
 * The risk this trades for is a device that repeats while a key is held:
 * that would now toggle mute for as long as a finger is on it. Nothing
 * in the captures does this -- seven seconds between a press and the
 * next line -- and the log is one line per report, so a device that
 * starts repeating says so immediately rather than presenting as a
 * flickering mute.
 */
/*
 * A boot keyboard report: 8 bytes, byte 0 modifiers, byte 1 reserved,
 * bytes 2..7 a set of currently-held keycodes.
 *
 * Edge detection is real here and is not optional. The array is a level
 * -- it says what is held, and it is re-sent while anything is -- so
 * acting on presence would repeat for as long as a key is down. A
 * keycode present now and absent from the previous report is a press.
 *
 * Byte 0 is NOT dispatched. It is Left Ctrl through Right GUI, and
 * reading it as a button table is exactly the bug this replaces.
 */
static void report_boot_keyboard(hid_ctx_t *ctx, const uint8_t *data, int len)
{
    if (len < 8) return;

    for (int i = 2; i < 8; i++) {
        const uint8_t code = data[i];
        if (code == 0 || code == 1 /* ErrorRollOver */) continue;

        bool was_held = false;
        for (int j = 0; j < 6; j++) {
            if (ctx->keys[j] == code) { was_held = true; break; }
        }
        if (was_held) continue;

        for (size_t k = 0; k < sizeof(KEYBOARD_KEYS) / sizeof(KEYBOARD_KEYS[0]); k++) {
            if (KEYBOARD_KEYS[k].code != code) continue;
            ESP_LOGI(TAG, "itf %u: key %s", ctx->itf_num, KEYBOARD_KEYS[k].name);
            if (s_cb) s_cb(KEYBOARD_KEYS[k].button);
            break;
        }
    }

    memcpy(ctx->keys, data + 2, sizeof(ctx->keys));
}

/*
 * A consumer-control report: a usage code, or zero for the release.
 *
 * Two shapes are in the wild and both are handled, because the report
 * descriptor was parsed for the usage page and not for the field layout
 * -- an honest limit rather than an oversight. An array-style collection
 * sends the usage itself, little endian, and a bitmap-style one sends a
 * bit per usage. The array form is what keyboards overwhelmingly use, so
 * that is what is decoded; the hex is logged either way, which is what
 * makes a device that does the other thing visible rather than silent.
 *
 * Zero is the release and dispatches nothing. That is the edge detection
 * -- unlike the remote in the bitmask path, a consumer collection does
 * send a clean release.
 */
static void report_consumer(hid_ctx_t *ctx, const uint8_t *data, int len)
{
    /* Skip the report ID when the descriptor declared any. */
    const int off = ctx->report_ids ? 1 : 0;
    if (len <= off) return;

    const uint16_t usage = (len - off >= 2)
        ? (uint16_t)(data[off] | ((uint16_t)data[off + 1] << 8))
        : (uint16_t)data[off];

    if (usage == 0) return;         /* release */

    for (size_t i = 0; i < sizeof(CONSUMER_USAGES) / sizeof(CONSUMER_USAGES[0]); i++) {
        if (CONSUMER_USAGES[i].usage != usage) continue;
        ESP_LOGI(TAG, "itf %u: consumer %s", ctx->itf_num, CONSUMER_USAGES[i].name);
        if (s_cb) s_cb(CONSUMER_USAGES[i].button);
        return;
    }

    /* Named by number rather than swallowed, for the same reason the
     * bitmask path names unknown bits: a key that does nothing and says
     * nothing looks identical to one whose reports are not arriving. */
    ESP_LOGI(TAG, "itf %u: consumer usage 0x%04X unmapped", ctx->itf_num, usage);
}

static void hid_report(hid_ctx_t *ctx, const uint8_t *data, int len)
{
    if (len <= 0) return;

    const uint8_t now = data[0];

    /*
     * The raw report and the names it resolved to, on one line, at info.
     *
     * Not behind a log level, deliberately. This mapping was arrived at
     * by reading these lines -- twice, after two wrong readings -- so the
     * evidence for it stays next to it. It is one line per press;
     * nothing that happens at the rate of a thumb needs rate limiting.
     */
    char hex[3 * HID_MAX_IN_REPORT + 1];
    int n = 0;
    for (int i = 0; i < len && i < HID_MAX_IN_REPORT; i++) {
        n += snprintf(hex + n, sizeof(hex) - n, "%02X ", data[i]);
    }

    char names[64];
    int m = 0;
    names[0] = '\0';
    for (int bit = 0; bit < 8; bit++) {
        if (!(now & (1 << bit))) continue;
        const int e = entry_for_bit(bit);
        if (e >= 0) {
            m += snprintf(names + m, sizeof(names) - m, "%s%s",
                          m ? "+" : "", BUTTON_BITS[e].name);
        } else {
            /* Named by number rather than swallowed. A button that does
             * nothing and says nothing is indistinguishable from one
             * whose reports are not arriving -- which is how the last two
             * faults in this file hid. */
            m += snprintf(names + m, sizeof(names) - m, "%sbit%d",
                          m ? "+" : "", bit);
        }
        if (m >= (int)sizeof(names) - 8) break;
    }

    ESP_LOGI(TAG, "remote: %s%s%s", hex, names[0] ? "-> " : "(release)", names);

    switch (ctx->kind) {
    case HID_KIND_BOOT_KEYBOARD:
        report_boot_keyboard(ctx, data, len);
        break;
    case HID_KIND_CONSUMER:
        report_consumer(ctx, data, len);
        break;
    case HID_KIND_BITMASK:
    default:
        for (int bit = 0; bit < 8; bit++) {
            if (now & (1 << bit)) dispatch_bit(ctx, bit);
        }
        break;
    }
}

/*
 * Do NOT resubmit from this callback.
 *
 * Carried over from the example along with the reason: it runs on the
 * client event task, and resubmitting inside it raced badly enough to
 * fail with ESP_ERR_INVALID_STATE on the first resubmit while leaving
 * the URB queued. Freeing the context at that point left later callbacks
 * reading freed memory, which showed up as an impossible interface
 * number in the log. The polling task owns the submit/free lifecycle.
 */
static void hid_transfer_cb(usb_transfer_t *transfer)
{
    hid_ctx_t *ctx = (hid_ctx_t *)transfer->context;

    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        hid_report(ctx, transfer->data_buffer, transfer->actual_num_bytes);
    }
    xSemaphoreGive(ctx->done);
}

static void hid_free(hid_ctx_t *ctx)
{
    usb_host_transfer_free(ctx->transfer);
    usb_host_interface_release(s_client, ctx->dev, ctx->itf_num);
    vSemaphoreDelete(ctx->done);
    free(ctx);
}

/*
 * One URB outstanding at a time: submit, wait for the completion, submit
 * again. Everything this task allocates it also frees, so there is one
 * owner and no lifetime ambiguity.
 */
static void hid_poll_task(void *arg)
{
    hid_ctx_t *ctx = (hid_ctx_t *)arg;
    usb_transfer_t *transfer = ctx->transfer;

    /* The first submit can report an error and still enqueue, so its
     * return is logged and not acted on -- the completion callback is
     * the authority on whether a URB is live. */
    esp_err_t ret = usb_host_transfer_submit(transfer);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "itf %u initial submit: %s", ctx->itf_num, esp_err_to_name(ret));
    }

    while (1) {
        /* Indefinitely. A remote sits silent for as long as nobody
         * touches it, so a timeout would mean nothing is wrong. */
        if (xSemaphoreTake(ctx->done, portMAX_DELAY) != pdTRUE) continue;

        if (transfer->status == USB_TRANSFER_STATUS_NO_DEVICE ||
            transfer->status == USB_TRANSFER_STATUS_CANCELED) {
            ESP_LOGI(TAG, "remote gone (itf %u)", ctx->itf_num);
            break;
        }
        if (transfer->status == USB_TRANSFER_STATUS_STALL) {
            ESP_LOGW(TAG, "itf %u endpoint stalled; polling stopped", ctx->itf_num);
            break;
        }

        ret = usb_host_transfer_submit(transfer);
        if (ret == ESP_ERR_NOT_FOUND || ret == ESP_ERR_INVALID_ARG) {
            /* Definitive rejection: nothing is in flight to wait for. */
            ESP_LOGI(TAG, "itf %u polling stopped (%s)", ctx->itf_num,
                     esp_err_to_name(ret));
            hid_free(ctx);
            vTaskDelete(NULL);
            return;
        }
    }

    /* Reached only when a completion said the device is gone or the
     * endpoint stalled, so no URB is outstanding and the free is safe. */
    hid_free(ctx);
    vTaskDelete(NULL);
}

/*
 * Ask the interface for its report descriptor and say what is in it.
 *
 * Synchronous from the caller's point of view, on a control transfer of
 * our own, at claim time -- before the polling task exists, so nothing
 * is racing it and a device that takes its time costs a slow enumeration
 * and nothing else.
 *
 * Returns false when the descriptor cannot be had, which is not an error
 * and is the case the fallback exists for: the headset remote stalls EP0
 * for exactly this request. The caller then classifies by protocol
 * alone.
 *
 * THE CONTEXT IS ON THE HEAP AND HAS TWO OWNERS. It used to be a local,
 * with the transfer freed and the semaphore deleted on the way out of
 * this function, and that was a boot loop waiting for a slow device.
 *
 * A stall is not the only way this request fails. The device can also
 * simply never answer, and then the wait below times out with the URB
 * still in flight -- at which point the old code deleted the semaphore,
 * freed the transfer, and returned, leaving `t->context` pointing at a
 * dead stack frame. When the transfer finally completed, the host
 * dispatched ctrl_cb() from usb_host_client_handle_events() and it gave
 * a deleted semaphore:
 *
 *   assert failed: xQueueGenericSend queue.c:936 (pxQueue)
 *   #4 usb_host_client_handle_events
 *   #5 client_task at hid.c
 *
 * It reproduced only with the device already plugged in at boot, where
 * enumeration lands on top of the SD mount and the first cover prefetch
 * and EP0 does not answer inside a second -- so it was a boot loop on
 * exactly the setup somebody would call normal use, and a clean run on
 * the setup somebody would call testing.
 *
 * Freeing an in-flight transfer is its own violation, and would be wrong
 * here even if nothing read the context afterwards.
 *
 * So: whoever finishes last cleans up. The abandoned flag is set under a
 * critical section and read under one, because the completion arrives on
 * the client task and the timeout happens on whichever task is claiming.
 */
#define HID_DESC_TYPE_REPORT    (0x22)
#define HID_REPORT_DESC_MAX     (512)

typedef struct {
    SemaphoreHandle_t done;
    usb_transfer_t   *transfer;
    bool              abandoned;   /* the waiter gave up; cb owns the free */
} ctrl_ctx_t;

static portMUX_TYPE s_ctrl_lock = portMUX_INITIALIZER_UNLOCKED;

/* Everything the pair allocates, released in one place so neither owner
 * has to remember the list. */
static void ctrl_ctx_free(ctrl_ctx_t *c)
{
    if (c->transfer) usb_host_transfer_free(c->transfer);
    if (c->done)     vSemaphoreDelete(c->done);
    free(c);
}

static void ctrl_cb(usb_transfer_t *t)
{
    ctrl_ctx_t *c = (ctrl_ctx_t *)t->context;

    bool mine;
    portENTER_CRITICAL(&s_ctrl_lock);
    mine = c->abandoned;
    portEXIT_CRITICAL(&s_ctrl_lock);

    if (mine) {
        /* The waiter is long gone and nothing is looking at this. The URB
         * has completed, so the transfer is no longer in flight and
         * freeing it here is the one moment when that is true. */
        ctrl_ctx_free(c);
        return;
    }

    xSemaphoreGive(c->done);
}

static bool report_desc_scan(usb_device_handle_t dev, uint8_t itf_num,
                             bool *has_consumer, bool *report_ids)
{
    *has_consumer = false;
    *report_ids = false;

    ctrl_ctx_t *c = calloc(1, sizeof(*c));
    if (!c) return false;

    if (usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + HID_REPORT_DESC_MAX,
                                0, &c->transfer) != ESP_OK) {
        free(c);
        return false;
    }

    c->done = xSemaphoreCreateBinary();
    if (!c->done) {
        ctrl_ctx_free(c);
        return false;
    }

    usb_transfer_t *t = c->transfer;

    usb_setup_packet_t *setup = (usb_setup_packet_t *)t->data_buffer;
    setup->bmRequestType = USB_BM_REQUEST_TYPE_DIR_IN |
                           USB_BM_REQUEST_TYPE_TYPE_STANDARD |
                           USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
    setup->bRequest = USB_B_REQUEST_GET_DESCRIPTOR;
    setup->wValue = (HID_DESC_TYPE_REPORT << 8);
    setup->wIndex = itf_num;
    setup->wLength = HID_REPORT_DESC_MAX;

    t->device_handle = dev;
    t->bEndpointAddress = 0;
    t->num_bytes = sizeof(usb_setup_packet_t) + HID_REPORT_DESC_MAX;
    t->callback = ctrl_cb;
    t->context = c;

    /* A failed submit never enqueued anything, so no callback is coming
     * and this side still owns everything. Distinguished from the
     * timeout below precisely because of that. */
    if (usb_host_transfer_submit_control(s_client, t) != ESP_OK) {
        ctrl_ctx_free(c);
        return false;
    }

    if (xSemaphoreTake(c->done, pdMS_TO_TICKS(1000)) != pdTRUE) {
        /*
         * Still in flight. Hand ownership to ctrl_cb() and leave without
         * touching anything it will read.
         *
         * The endpoint is halted and flushed first so the URB is retired
         * now rather than landing at an arbitrary later moment -- that
         * turns "the callback runs eventually, on a context that is
         * still valid" into "the callback runs shortly, and then this is
         * over". The clear puts EP0 back in a usable state for whatever
         * asks next, since a halted control endpoint would fail every
         * subsequent request on this device.
         *
         * If any of the three fail there is nothing useful to do: the
         * context stays alive and owned by the callback, which is the
         * safe direction to fail in -- a leaked 550 bytes rather than a
         * panic.
         */
        portENTER_CRITICAL(&s_ctrl_lock);
        c->abandoned = true;
        portEXIT_CRITICAL(&s_ctrl_lock);

        ESP_LOGD(TAG, "itf %u report descriptor timed out; abandoning the URB",
                 itf_num);

        usb_host_endpoint_halt(dev, 0);
        usb_host_endpoint_flush(dev, 0);
        usb_host_endpoint_clear(dev, 0);
        return false;
    }

    bool ok = false;
    if (t->status == USB_TRANSFER_STATUS_COMPLETED) {

        const uint8_t *d = t->data_buffer + sizeof(usb_setup_packet_t);
        int n = t->actual_num_bytes - (int)sizeof(usb_setup_packet_t);
        if (n > HID_REPORT_DESC_MAX) n = HID_REPORT_DESC_MAX;

        /*
         * A short-item walk, and only a walk. HID 1.11 section 6.2.2:
         * every short item is a one-byte prefix whose low two bits are
         * the data length, with 3 meaning 4 bytes, followed by that
         * data. Skipping by that length is enough to visit every item
         * without understanding any of them.
         *
         * Long items (prefix 0xFE) carry their own size in the next
         * byte. They are vanishingly rare and handled so the walk
         * cannot desynchronise on one.
         *
         * Two items are looked for: Usage Page (Global, tag 0, prefix
         * 0x05) with a value of 0x0C, which is the Consumer page; and
         * Report ID (Global, tag 8, prefix 0x85), whose presence means
         * every report carries an ID byte in front.
         */
        for (int i = 0; i < n; ) {
            const uint8_t prefix = d[i];

            if (prefix == 0xFE) {                 /* long item */
                if (i + 1 >= n) break;
                i += 3 + d[i + 1];
                continue;
            }

            int size = prefix & 0x03;
            if (size == 3) size = 4;

            if (prefix == 0x05 && size == 1 && i + 1 < n && d[i + 1] == 0x0C) {
                *has_consumer = true;
            }
            if ((prefix & 0xFC) == 0x84) {        /* Report ID, any size */
                *report_ids = true;
            }

            i += 1 + size;
        }
        ok = true;
    }

    /* The completion has already run and was not abandoned, so this side
     * is the sole owner and frees. */
    ctrl_ctx_free(c);
    return ok;
}

static esp_err_t hid_claim(usb_device_handle_t dev, const usb_intf_desc_t *intf,
                           const usb_config_desc_t *cfg)
{
    const usb_ep_desc_t *ep = NULL;
    int ep_offset = 0;
    for (int i = 0; i < intf->bNumEndpoints; i++) {
        int off = ep_offset;
        const usb_ep_desc_t *cand =
            usb_parse_endpoint_descriptor_by_index(intf, i, cfg->wTotalLength, &off);
        if (!cand) continue;
        ep_offset = off;
        if (USB_EP_DESC_GET_XFERTYPE(cand) == USB_TRANSFER_TYPE_INTR &&
            USB_EP_DESC_GET_EP_DIR(cand)) {
            ep = cand;
            break;
        }
    }
    if (!ep) return ESP_ERR_NOT_FOUND;

    ESP_RETURN_ON_ERROR(usb_host_interface_claim(s_client, dev,
                                                 intf->bInterfaceNumber, 0),
                        TAG, "claim");

    hid_ctx_t *ctx = calloc(1, sizeof(hid_ctx_t));
    if (!ctx) {
        usb_host_interface_release(s_client, dev, intf->bInterfaceNumber);
        return ESP_ERR_NO_MEM;
    }
    ctx->dev = dev;
    ctx->itf_num = intf->bInterfaceNumber;

    /*
     * Classify once, here, rather than guessing per report.
     *
     * Protocol first: bInterfaceProtocol 1 is the boot keyboard, and
     * that is stated by the interface descriptor rather than inferred,
     * so it wins outright. Otherwise ask for the report descriptor; a
     * Consumer page means the media keys are on this interface. A device
     * that will not answer keeps the old bitmask reading, which is what
     * the headset remote needs and what it has always had.
     */
    bool has_consumer = false, report_ids = false;
    if (intf->bInterfaceProtocol == 1) {
        ctx->kind = HID_KIND_BOOT_KEYBOARD;
    } else if (report_desc_scan(dev, intf->bInterfaceNumber,
                                &has_consumer, &report_ids) && has_consumer) {
        ctx->kind = HID_KIND_CONSUMER;
        ctx->report_ids = report_ids;
    } else {
        ctx->kind = HID_KIND_BITMASK;
    }

    ctx->done = xSemaphoreCreateBinary();
    if (!ctx->done) {
        free(ctx);
        usb_host_interface_release(s_client, dev, intf->bInterfaceNumber);
        return ESP_ERR_NO_MEM;
    }

    const uint16_t mps = USB_EP_DESC_GET_MPS(ep);
    esp_err_t ret = usb_host_transfer_alloc(mps, 0, &ctx->transfer);
    if (ret != ESP_OK) {
        vSemaphoreDelete(ctx->done);
        free(ctx);
        usb_host_interface_release(s_client, dev, intf->bInterfaceNumber);
        return ret;
    }

    ctx->transfer->device_handle = dev;
    ctx->transfer->bEndpointAddress = ep->bEndpointAddress;
    ctx->transfer->num_bytes = mps;
    ctx->transfer->callback = hid_transfer_cb;
    ctx->transfer->context = ctx;

    /* Priority 4, the same as the UI task: a button press is a UI event
     * and neither should be able to sit behind the other. Below the
     * audio writer at 6, which is the only thing here that must never
     * wait. */
    if (xTaskCreate(hid_poll_task, "hid_poll", 3072, ctx, 4, NULL) != pdPASS) {
        usb_host_transfer_free(ctx->transfer);
        vSemaphoreDelete(ctx->done);
        free(ctx);
        usb_host_interface_release(s_client, dev, intf->bInterfaceNumber);
        return ESP_ERR_NO_MEM;
    }

    static const char *const KIND_NAME[] = { "bitmask", "boot keyboard", "consumer" };
    ESP_LOGI(TAG, "remote on itf %u: EP 0x%02X, %u byte reports, %u ms, %s%s",
             intf->bInterfaceNumber, ep->bEndpointAddress, mps, ep->bInterval,
             KIND_NAME[ctx->kind], ctx->report_ids ? ", report IDs" : "");
    return ESP_OK;
}

static void inspect(usb_device_handle_t dev)
{
    const usb_config_desc_t *cfg = NULL;
    if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK) return;

    int offset = 0;
    for (int i = 0; i < cfg->bNumInterfaces; i++) {
        const usb_intf_desc_t *intf = usb_parse_interface_descriptor(cfg, i, 0, &offset);
        if (!intf || intf->bInterfaceClass != USB_CLASS_HID) continue;

        const esp_err_t ret = hid_claim(dev, intf, cfg);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "itf %u: could not poll (%s)",
                     intf->bInterfaceNumber, esp_err_to_name(ret));
        }
    }
}

/*
 * Every device on the bus is offered to this client, including the ones
 * the MSC and UAC drivers are going to take. That is how the host
 * library works -- clients see all devices and claim the interfaces they
 * want -- and it is why inspect() filters on bInterfaceClass rather than
 * assuming anything about what turned up.
 *
 * The device is opened and left open for as long as it is attached. The
 * close is on DEV_GONE, and the polling tasks tear themselves down off
 * their own completion status rather than being told, which is the one
 * ordering that does not require this callback to know how many of them
 * there are.
 */
/*
 * Open handles, one slot per device this client has opened.
 *
 * This was a single `static usb_device_handle_t s_open` and that is one
 * device's worth of state for a bus that routinely has two things on it
 * -- a mass storage stick and a headset, which is the ordinary case here
 * and is in every log. The second NEW_DEV overwrote the first handle,
 * leaking it, and the next DEV_GONE closed whichever happened to be
 * current rather than the one that actually left. Closing a device out
 * from under a live interface claim is the same class of fault as the
 * one in report_desc_scan() above, arrived at from the other direction.
 *
 * DEV_GONE carries the handle that left, so the close does not have to
 * guess. Four slots: this client only ever opens devices to look at
 * their interfaces, and a hub full of them is a case the port cannot
 * power anyway (see the README on hubs).
 */
#define HID_MAX_OPEN_DEVS   (4)

static usb_device_handle_t s_open[HID_MAX_OPEN_DEVS];

static void open_remember(usb_device_handle_t dev)
{
    for (int i = 0; i < HID_MAX_OPEN_DEVS; i++) {
        if (!s_open[i]) { s_open[i] = dev; return; }
    }
    /* Nothing to do but say so. The device stays open and its interfaces
     * keep working; it is the close on unplug that will be missed. */
    ESP_LOGW(TAG, "more than %d open devices; not tracking this one",
             HID_MAX_OPEN_DEVS);
}

static void open_forget_and_close(usb_device_handle_t dev)
{
    for (int i = 0; i < HID_MAX_OPEN_DEVS; i++) {
        if (s_open[i] == dev) {
            usb_host_device_close(s_client, dev);
            s_open[i] = NULL;
            return;
        }
    }
    /* Not found. DEV_GONE only reaches clients that opened the device,
     * so this means the open failed earlier or the slot table was full
     * when it was opened -- both already logged where they happened. */
}

static void client_event_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    (void)arg;

    switch (msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV: {
        usb_device_handle_t dev = NULL;
        if (usb_host_device_open(s_client, msg->new_dev.address, &dev) == ESP_OK) {
            open_remember(dev);
            inspect(dev);
        }
        break;
    }

    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        open_forget_and_close(msg->dev_gone.dev_hdl);
        break;

    default:
        break;
    }
}

static void client_task(void *arg)
{
    (void)arg;
    while (1) {
        usb_host_client_handle_events(s_client, portMAX_DELAY);
    }
}

/* Called by usbhost.c on the bus task, after usb_host_install() and
 * before VBUS -- the same slot the MSC and UAC class drivers use. This
 * is a plain host client rather than a class driver, but the ordering
 * requirement is identical: registered before power, or it misses the
 * enumeration of whatever is already plugged in. */
static esp_err_t hid_client_install(void)
{
    const usb_host_client_config_t cfg = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = NULL,
        },
    };
    ESP_RETURN_ON_ERROR(usb_host_client_register(&cfg, &s_client), TAG, "register");

    if (xTaskCreate(client_task, "hid_client", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t hid_init(hid_button_cb_t cb)
{
    s_cb = cb;
    return usbhost_register_class("hid", hid_client_install);
}
