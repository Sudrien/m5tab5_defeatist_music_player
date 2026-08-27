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
 * Which bit of report byte 0 is which button, and whether that button
 * might be a latching switch.
 *
 * THE BIT NUMBERS ARE AN INFERENCE, NOT A READING. The device stalls EP0
 * for its report descriptor, so there is nothing to parse and no way to
 * derive them. They are the usual Consumer Control page ordering --
 * Volume Increment 0xE9, Volume Decrement 0xEA, Mute 0xE2 -- laid out
 * one bit per button in the first byte, which is what these remotes
 * almost always are.
 *
 * So: if a button does the wrong thing, this is a table edit and not a
 * mystery. Every change to byte 0 is logged as hex next to the names it
 * resolved to, at info level, precisely so the two can be compared
 * without going and getting another build:
 *
 *   remote: 01 00 00 00 -> Vol+
 *   remote: 00 00 00 00
 *
 * A bit with no entry is logged as "bitN" and does nothing.
 *
 * `toggle` is a different kind of claim from the bit number. It does not
 * say the button IS latching -- it says this button is one whose second
 * edge could mean something, so measure it rather than assume. See
 * dispatch_bit(). Volume keys are false: they are momentary by
 * construction, nobody ships a latching volume-up, and measuring them
 * would turn a long press into a spurious extra step.
 */
static const struct {
    uint8_t      bit;
    hid_button_t button;
    const char  *name;
    bool         toggle;
} BUTTON_BITS[] = {
    { 0, HID_BTN_VOL_UP,   "Vol+",    false },
    { 1, HID_BTN_VOL_DOWN, "Vol-",    false },
    { 2, HID_BTN_MUTE,     "Mute",    true  },
    { 3, HID_BTN_MIC_MUTE, "MicMute", true  },
};

/*
 * How long a toggle-capable bit has to stay set before its release is
 * read as a second press rather than as letting go.
 *
 * This is the whole of the discrimination, and it is a timing threshold
 * because nothing else distinguishes the two devices. A momentary button
 * whose state the software tracks and a latching switch that reports its
 * own state emit the SAME bytes -- 04 then 00 -- and differ only in
 * when. A thumb releases a button in well under 400 ms. A latched
 * switch sits there until somebody comes back to it, which is at best
 * the better part of a second later and usually many.
 *
 * Getting it wrong in either direction is survivable and visible: a
 * momentary press held past the threshold fires twice and cancels out,
 * a latching switch flicked faster than it fires once and inverts. Both
 * say so in the log, which is why the decision is logged rather than
 * only acted on.
 */
#define LATCH_HOLD_MS   (400)

typedef enum {
    MODE_UNKNOWN = 0,
    MODE_MOMENTARY,     /* released quickly; software owns the state */
    MODE_LATCHING,      /* held until pressed again; the switch owns it */
} bit_mode_t;

typedef struct {
    usb_device_handle_t dev;
    usb_transfer_t     *transfer;
    SemaphoreHandle_t   done;
    uint8_t itf_num;
    uint8_t prev0;              /* byte 0 of the last report */
    bool    have_prev;

    /* Per-bit, because one remote can carry both kinds: the volume keys
     * on this headset are momentary and the mute may not be. */
    TickType_t rise_tick[8];
    bit_mode_t mode[8];
} hid_ctx_t;

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
 * One edge on one bit.
 *
 * A rising edge is always a press and always dispatches. What to do with
 * the falling edge is the question this file could not answer by
 * inspection, because the two possibilities are byte-identical:
 *
 *   momentary + software state   press -> 04, release -> 00
 *   latching switch              press -> 04, press again -> 00
 *
 * Both send 04 and then 00. The only thing that differs is how long 04
 * lasts, so that is what is measured. Under LATCH_HOLD_MS the release is
 * a thumb coming off a button and is ignored -- the player already
 * toggled on the press. Over it, the bit was sitting latched and the
 * return to 0 is the second press, so it dispatches.
 *
 * The decision is logged the first time it is made for each bit, because
 * a mute button that fires twice per press and a mute button that fires
 * once look the same from the outside -- silent -- and this line is what
 * separates them.
 *
 * Bits not marked `toggle` never dispatch on release at all. A volume
 * key held for a second is a person deciding, not two presses.
 */
static void dispatch_bit(hid_ctx_t *ctx, int bit, bool rising)
{
    const int e = entry_for_bit(bit);
    if (e < 0) return;

    if (rising) {
        ctx->rise_tick[bit] = xTaskGetTickCount();
        if (s_cb) s_cb(BUTTON_BITS[e].button);
        return;
    }

    if (!BUTTON_BITS[e].toggle) return;

    const uint32_t held_ms =
        (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount() - ctx->rise_tick[bit]);
    const bool latching = held_ms >= LATCH_HOLD_MS;
    const bit_mode_t mode = latching ? MODE_LATCHING : MODE_MOMENTARY;

    if (ctx->mode[bit] != mode) {
        ctx->mode[bit] = mode;
        ESP_LOGI(TAG, "%s is %s (held %lu ms); release %s a second press",
                 BUTTON_BITS[e].name,
                 latching ? "a latching switch" : "momentary",
                 (unsigned long)held_ms,
                 latching ? "counts as" : "does not count as");
    }

    if (latching && s_cb) s_cb(BUTTON_BITS[e].button);
}

static void hid_report(hid_ctx_t *ctx, const uint8_t *data, int len)
{
    if (len <= 0) return;

    const uint8_t now = data[0];
    const uint8_t was = ctx->have_prev ? ctx->prev0 : 0;
    const bool first = !ctx->have_prev;

    ctx->prev0 = now;
    ctx->have_prev = true;

    /* Repeats while a button is held carry nothing and would be one line
     * per poll interval. Only changes are logged or acted on. */
    if (!first && now == was) return;

    const uint8_t changed = (uint8_t)(now ^ was);
    if (!changed) return;

    /*
     * The raw report and the names it resolved to, on one line, at info.
     *
     * Not behind a log level, deliberately. BUTTON_BITS is an inference,
     * so the first thing anyone needs from this file is the evidence
     * that it is right -- and putting that at debug means fetching
     * another build to answer a question the running one already knew.
     * It is one line per press; nothing that happens at the rate of a
     * thumb needs rate limiting.
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
        if (!(changed & (1 << bit))) continue;
        const bool rising = (now & (1 << bit)) != 0;
        const int e = entry_for_bit(bit);
        m += snprintf(names + m, sizeof(names) - m, "%s%s%s",
                      m ? " " : "",
                      rising ? "+" : "-",
                      e >= 0 ? BUTTON_BITS[e].name : "bit?");
        if (e < 0) {
            /* Named by number rather than swallowed. A button that does
             * nothing and says nothing is indistinguishable from one
             * whose reports are not arriving, and those have completely
             * different fixes. */
            m += snprintf(names + m, sizeof(names) - m, "%d", bit);
        }
        if (m >= (int)sizeof(names) - 8) break;
    }

    ESP_LOGI(TAG, "remote: %s%s%s", hex, names[0] ? "-> " : "", names);

    for (int bit = 0; bit < 8; bit++) {
        if (!(changed & (1 << bit))) continue;
        dispatch_bit(ctx, bit, (now & (1 << bit)) != 0);
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

    ESP_LOGI(TAG, "remote on itf %u: EP 0x%02X, %u byte reports, %u ms",
             intf->bInterfaceNumber, ep->bEndpointAddress, mps, ep->bInterval);
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
static void client_event_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    (void)arg;
    static usb_device_handle_t s_open;

    switch (msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        if (usb_host_device_open(s_client, msg->new_dev.address, &s_open) == ESP_OK) {
            inspect(s_open);
        }
        break;

    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        if (s_open) {
            usb_host_device_close(s_client, s_open);
            s_open = NULL;
        }
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
