/*
 * uac.c -- USB Audio Class output.
 *
 * Adapted from m5tab5_esp_idf_usb_host_example's uac_example.c, which
 * opens both directions and loops the microphone into the speaker. The
 * loopback and the whole RX half are gone; what is kept is the part that
 * was actually load-bearing there -- that a headset is two logical UAC
 * devices, that alternate setting 0 is always the zero-bandwidth idle
 * setting so the usable ones start at 1, and that the connect callback
 * fires per interface rather than per device.
 *
 * What is new is that the format is not the device's choice any more.
 * The example took the first 16-bit PCM alternate and its first listed
 * rate, which is fine when the only requirement is that both ends agree
 * with each other. Here the rate is set by the file being played, so the
 * search runs the other way: the caller states a rate and a channel
 * count, and this either finds an alternate that offers exactly that or
 * says it cannot.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "usb/uac_host.h"

#include "battery.h"
#include "uac.h"
#include "usbhost.h"

static const char *TAG = "tab5_uac";

/*
 * The driver's own ring, between uac_write() and the isochronous
 * transfers.
 *
 * 16 KB is about 93 ms of 44.1 kHz stereo. It does not need to be large:
 * the PCM ring in player.c is the buffer that absorbs a card read, and
 * this one only has to cover the gap between the writer task being
 * scheduled and the next USB frame. Making it large would add latency to
 * every transport button for no benefit, since a press is serviced by
 * resetting the ring upstream of here.
 *
 * The threshold is what the driver uses to decide the stream has drained
 * far enough to be worth a TX_DONE event. Nothing reacts to those here
 * -- the writer task fills on its own schedule -- so it only wants to be
 * a sane fraction of the buffer.
 */
#define UAC_BUFFER_SIZE         (16 * 1024)
#define UAC_BUFFER_THRESHOLD    (4 * 1024)

#define EVENT_QUEUE_DEPTH       (4)

/*
 * The queue carries this rather than a uac_host_driver_event_t.
 *
 * The two enums the driver hands out overlap in name and not in type.
 * Connections arrive as uac_host_driver_event_t on the driver callback
 * (RX_CONNECTED, TX_CONNECTED); the disconnect arrives as
 * uac_host_device_event_t on the *device* callback, and
 * UAC_HOST_DRIVER_EVENT_DISCONNECTED -- despite the name -- is a value
 * of the latter. Putting all three in one field typed as the former is
 * a -Wswitch error, correctly: the compiler is pointing out that a case
 * label is not a value the type can hold.
 *
 * So this file names the three things it actually queues and the two
 * callbacks translate on the way in. That also decouples the queue from
 * whichever enum upstream decides these belong to next.
 */
typedef enum {
    MSG_ATTACH_TX = 0,      /* a speaker interface appeared   */
    MSG_ATTACH_RX,          /* a microphone interface appeared */
    MSG_DETACH,             /* the open device went away      */
} uac_msg_kind_t;

typedef struct {
    uint8_t        addr;
    uint8_t        iface_num;
    uac_msg_kind_t kind;
} uac_queue_msg_t;

static QueueHandle_t s_queue;

/*
 * s_dev is shared, and this is the one place in this program where a
 * handle crosses a task boundary rather than a published value.
 *
 * It cannot be avoided: writing audio means calling the driver with the
 * handle, and the writer is not the task that opens or closes it. So it
 * is under a mutex, and the rule is that s_dev is only ever read or
 * written with s_lock held -- including by the event task, which is the
 * one that closes it. Closing a device while a writer is inside
 * uac_host_device_write() on it is exactly the use-after-free the PCM
 * ring's history in CLAUDE.md is about.
 *
 * The disconnect callback therefore does not close anything. It queues,
 * the same way the connect callback does, and the event task performs
 * the close with the lock held. That costs a disconnect the length of
 * one in-flight write -- bounded by the caller's timeout -- and buys the
 * property that no handle is ever freed while it is in use.
 *
 * s_present is the published value that everything outside this file
 * reads, so uac_present() does not have to take the lock and cannot
 * block the UI on a write in flight.
 */
static SemaphoreHandle_t s_lock;
static uac_host_device_handle_t s_dev;
static volatile bool     s_present;
static volatile bool     s_streaming;
static volatile uint32_t s_generation;

/* The format currently streaming, so a repeat request for the same
 * format is a no-op rather than a stop/start -- which on a real device
 * is an audible gap at every track boundary in an album that is all one
 * rate. */
static uint32_t s_rate;
static uint8_t  s_channels;

static char s_product[64];

/*
 * Attach time and error count, for the line printed on the way out.
 *
 * A USB audio device that drops off the bus after a while leaves almost
 * nothing behind: the driver logs a pipe that is no longer active, which
 * is the symptom rather than the cause, and by the time anything here
 * runs the device is already gone. These two numbers plus the pack
 * voltage are what distinguish the three things it can be -- a rail
 * sagging under the device's own draw, a device failing on its own
 * schedule, and a stream this code stopped feeding -- and none of them
 * are distinguishable from "HCD Pipe not in active state" alone.
 *
 * Not a fix. This is the instrumentation that says which fix to write.
 */
static int64_t  s_attach_us;
static uint32_t s_xfer_errors;

/*
 * The volume the UI last asked for, and whether the device turned out to
 * have a control for it.
 *
 * A published pair, written by anyone and applied by the event task.
 * uac_set_volume() used to do the control transfer on its caller's task,
 * which is the UI task -- so a volume drag put fifty attempts a second
 * behind the mutex the audio writer holds for the length of a write, in
 * the loop that also dispatches play, next, seek and the folder button.
 * That is a self-inflicted stall in the one task that must not stall.
 *
 * s_hw_volume is false until the first attempt has been made and false
 * forever once one has failed, so the software-gain fallback is decided
 * once rather than probed per change.
 */
static volatile uint8_t s_want_volume = 50;
static volatile bool    s_volume_dirty;
static volatile bool    s_hw_volume;

bool     uac_present(void)    { return s_present; }
bool     uac_streaming(void)  { return s_streaming; }
uint32_t uac_generation(void) { return s_generation; }
const char *uac_product(void) { return s_product; }

/* ------------------------------------------------------------------ */
/* Format selection                                                    */
/* ------------------------------------------------------------------ */

static bool alt_offers_rate(const uac_host_dev_alt_param_t *p, uint32_t rate)
{
    /* sample_freq_type 0 means a continuous range rather than a list.
     * Rare on the cheap parts and common on better DACs. */
    if (p->sample_freq_type == 0) {
        return rate >= p->sample_freq_lower && rate <= p->sample_freq_upper;
    }
    for (uint8_t i = 0; i < p->sample_freq_type; i++) {
        if (p->sample_freq[i] == rate) return true;
    }
    return false;
}

/*
 * The first alternate that offers exactly this rate, this channel count
 * and 16-bit PCM.
 *
 * "Exactly" is the whole point. The example picked the first usable
 * alternate and took whatever rate it listed first, because it was
 * looping one interface into another and only needed them to agree. A
 * player has a rate already -- the file's -- and handing 44.1 kHz data
 * to a device streaming at 48 kHz plays it a semitone flat and 9% fast.
 * That reads as a broken player. Declining reads as an unsupported
 * device, which is what it is, and the caller has somewhere else to go.
 *
 * Alternates start at 1: alternate 0 on a UAC Audio Streaming interface
 * is always the zero-bandwidth "off" setting.
 */
static esp_err_t pick_alt(uac_host_device_handle_t dev, uint8_t alt_count,
                          uint32_t rate, uint8_t channels,
                          uint8_t *out_alt, uac_host_dev_alt_param_t *out_param)
{
    for (uint8_t alt = 1; alt <= alt_count; alt++) {
        uac_host_dev_alt_param_t p;
        if (uac_host_get_device_alt_param(dev, alt, &p) != ESP_OK) continue;
        if (p.format != 1 /* PCM */ || p.bit_resolution != 16) continue;
        if (p.channels != channels) continue;
        if (!alt_offers_rate(&p, rate)) continue;

        *out_alt = alt;
        *out_param = p;
        return ESP_OK;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

/* ------------------------------------------------------------------ */
/* Reporting                                                           */
/* ------------------------------------------------------------------ */

/* String descriptors on this driver's info struct are already wide
 * chars, pre-truncated to UAC_STR_DESC_MAX_LENGTH. Flattened to the
 * ASCII subset the same way the USB host example does, because the
 * places this string ends up -- the log and the format card -- are both
 * happier with a '?' than with a codepoint nothing here can draw. */
static void flatten(char *out, size_t out_len, const wchar_t *ws)
{
    size_t n = 0;
    if (ws) {
        for (; ws[n] != L'\0' && n + 1 < out_len && n < UAC_STR_DESC_MAX_LENGTH; n++) {
            const wchar_t c = ws[n];
            out[n] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
        }
    }
    out[n] = '\0';
}

static void report(uac_host_device_handle_t dev, const uac_host_dev_info_t *info)
{
    ESP_LOGI(TAG, "USB audio output attached (itf %u, addr %u)",
             info->iface_num, info->addr);
    ESP_LOGI(TAG, "  %-12s %04X:%04X", "VID:PID", info->VID, info->PID);
    if (s_product[0]) ESP_LOGI(TAG, "  %-12s %s", "product", s_product);
    /* The pack at the moment a device arrives, so the reading on the way
     * out has something to be compared against. */
    ESP_LOGI(TAG, "  %-12s %d mV", "pack", battery_mv());

    for (uint8_t alt = 1; alt <= info->iface_alt_num; alt++) {
        uac_host_dev_alt_param_t p;
        if (uac_host_get_device_alt_param(dev, alt, &p) != ESP_OK) continue;
        if (p.sample_freq_type == 0) {
            ESP_LOGI(TAG, "  alt %u: %u ch, %u-bit, %lu-%lu Hz (continuous)",
                     alt, p.channels, p.bit_resolution,
                     (unsigned long)p.sample_freq_lower,
                     (unsigned long)p.sample_freq_upper);
        } else {
            char rates[96];
            int off = 0;
            for (uint8_t i = 0; i < p.sample_freq_type && off < (int)sizeof(rates) - 12; i++) {
                off += snprintf(rates + off, sizeof(rates) - off, "%s%lu",
                                i ? " " : "", (unsigned long)p.sample_freq[i]);
            }
            ESP_LOGI(TAG, "  alt %u: %u ch, %u-bit, %s Hz",
                     alt, p.channels, p.bit_resolution, rates);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Streaming                                                           */
/* ------------------------------------------------------------------ */

esp_err_t uac_stream_start(uint32_t rate, uint8_t channels)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);

    esp_err_t ret = ESP_ERR_NOT_FOUND;
    if (!s_dev) goto out;

    /* Same format already running: say yes without touching the device.
     * A stop/start here is an audible gap, and on an album that is all
     * one rate the caller asks once per track. */
    if (s_streaming && rate == s_rate && channels == s_channels) {
        ret = ESP_OK;
        goto out;
    }

    if (s_streaming) {
        uac_host_device_stop(s_dev);
        s_streaming = false;
    }

    uac_host_dev_info_t info;
    ret = uac_host_get_device_info(s_dev, &info);
    if (ret != ESP_OK) goto out;

    uint8_t alt = 0;
    uac_host_dev_alt_param_t p;
    ret = pick_alt(s_dev, info.iface_alt_num, rate, channels, &alt, &p);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "device offers no %lu Hz %u ch 16-bit setting",
                 (unsigned long)rate, channels);
        goto out;
    }

    const uac_host_stream_config_t cfg = {
        .channels = channels,
        .bit_resolution = 16,
        .sample_freq = rate,
        .flags = 0,
    };
    ret = uac_host_device_start(s_dev, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "stream start failed (%s)", esp_err_to_name(ret));
        goto out;
    }

    s_rate = rate;
    s_channels = channels;
    s_streaming = true;
    ESP_LOGI(TAG, "streaming: alt %u, %u ch, 16-bit, %lu Hz",
             alt, channels, (unsigned long)rate);

out:
    xSemaphoreGive(s_lock);
    return ret;
}

void uac_stream_stop(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_dev && s_streaming) {
        uac_host_device_stop(s_dev);
        s_rate = 0;
        s_channels = 0;
    }
    s_streaming = false;
    xSemaphoreGive(s_lock);
}

esp_err_t uac_write(const void *data, size_t len, uint32_t timeout_ms)
{
    if (!s_lock || !data || len == 0) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    if (s_dev && s_streaming) {
        ret = uac_host_device_write(s_dev, (uint8_t *)data, (uint32_t)len,
                                    pdMS_TO_TICKS(timeout_ms));
    }
    xSemaphoreGive(s_lock);
    return ret;
}

bool uac_has_volume_control(void) { return s_hw_volume; }

/* Publishes and returns. Two volatile stores; no lock, no transfer, no
 * possibility of blocking behind the writer. */
void uac_set_volume(uint8_t percent)
{
    s_want_volume = (percent > 100) ? 100 : percent;
    s_volume_dirty = true;
}

/* The other half, on the event task. Tried once per attach; a failure
 * latches the software-gain fallback rather than being retried, because
 * the answer cannot change while the same device is attached. */
static void apply_volume(void)
{
    static bool tried_and_failed;

    /*
     * Nothing to set the volume on until the stream is running.
     *
     * handle_connect() raises the dirty flag so the UI's level lands on
     * a newly attached device, but the interface is opened at alternate
     * 0 and only resumed when a track states its format -- so between
     * those two moments the driver correctly refuses:
     *
     *   E uac-host: uac_host_device_set_volume(2632):
     *               device not ready or active
     *
     * The retry a moment later worked, so this was noise rather than a
     * fault, and it looked exactly like the failure that latches the
     * software-gain fallback. Left set rather than cleared: the request
     * is still outstanding and the next pass is 50 ms away.
     */
    if (!s_streaming) return;

    s_volume_dirty = false;
    if (tried_and_failed) return;

    const uint8_t want = s_want_volume;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    if (s_dev) ret = uac_host_device_set_volume(s_dev, want);
    xSemaphoreGive(s_lock);

    if (ret == ESP_OK) {
        s_hw_volume = true;
    } else if (ret != ESP_ERR_INVALID_STATE) {
        tried_and_failed = true;
        s_hw_volume = false;
        ESP_LOGI(TAG, "no device volume control; gain applied in software");
    }
}

/* ------------------------------------------------------------------ */
/* Events                                                              */
/* ------------------------------------------------------------------ */

static void device_event_cb(uac_host_device_handle_t dev,
                            const uac_host_device_event_t event, void *arg)
{
    (void)arg;
    switch (event) {
    case UAC_HOST_DRIVER_EVENT_DISCONNECTED:
        /* Not closed here. This runs on the driver's task and the writer
         * may be inside uac_host_device_write() on this handle right
         * now; closing it under them is a use-after-free. Published
         * first so nothing new starts a write, then queued so the event
         * task can close it with the lock held. */
        s_present = false;
        s_streaming = false;
        s_generation++;
        /* Printed here rather than in handle_disconnect(), because this
         * runs at the moment the device went away and that runs after
         * the writer has finished whatever it was in the middle of. */
        ESP_LOGW(TAG, "device dropped after %d ms, %lu transfer errors, pack %d mV",
                 (int)((esp_timer_get_time() - s_attach_us) / 1000),
                 (unsigned long)s_xfer_errors, battery_mv());
        {
            const uac_queue_msg_t msg = {
                .addr = 0, .iface_num = 0, .kind = MSG_DETACH,
            };
            xQueueSend(s_queue, &msg, 0);
        }
        break;

    case UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR:
        /* Rate limited: an isochronous endpoint that has started failing
         * fails every frame, and a thousand identical lines a second
         * buries the thing that caused it. The first few and then every
         * 256th, which is enough to tell "one glitch" from "it never
         * recovered". */
        s_xfer_errors++;
        if (s_xfer_errors <= 4 || (s_xfer_errors % 256) == 0) {
            ESP_LOGW(TAG, "transfer error (%lu so far, %d ms in, pack %d mV)",
                     (unsigned long)s_xfer_errors,
                     (int)((esp_timer_get_time() - s_attach_us) / 1000),
                     battery_mv());
        }
        break;

    default:
        /* TX_DONE only means the driver's ring crossed its threshold.
         * The writer fills on its own schedule and has nothing to do
         * with this. */
        break;
    }
}

static void driver_event_cb(uint8_t addr, uint8_t iface_num,
                            const uac_host_driver_event_t event, void *arg)
{
    (void)arg;
    /* Runs on the UAC driver's background task; opening a device here
     * would block it, the same reasoning as msc_event_cb(). */
    if (event != UAC_HOST_DRIVER_EVENT_RX_CONNECTED &&
        event != UAC_HOST_DRIVER_EVENT_TX_CONNECTED) {
        return;
    }
    const uac_queue_msg_t msg = {
        .addr = addr,
        .iface_num = iface_num,
        .kind = (event == UAC_HOST_DRIVER_EVENT_TX_CONNECTED) ? MSG_ATTACH_TX
                                                              : MSG_ATTACH_RX,
    };
    xQueueSend(s_queue, &msg, 0);
}

static void handle_connect(uint8_t addr, uint8_t iface_num)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);

    /* One output at a time. A second speaker interface on the same
     * device -- or a second device -- is left closed rather than
     * arbitrated: there is one pair of ears and no way to ask which. */
    if (s_dev) {
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "second output interface (itf %u) left closed", iface_num);
        return;
    }

    const uac_host_device_config_t cfg = {
        .addr = addr,
        .iface_num = iface_num,
        .buffer_size = UAC_BUFFER_SIZE,
        .buffer_threshold = UAC_BUFFER_THRESHOLD,
        .callback = device_event_cb,
        .callback_arg = NULL,
    };

    uac_host_device_handle_t dev = NULL;
    const esp_err_t err = uac_host_device_open(&cfg, &dev);
    if (err != ESP_OK) {
        xSemaphoreGive(s_lock);
        ESP_LOGE(TAG, "could not open output interface: %s", esp_err_to_name(err));
        return;
    }

    uac_host_dev_info_t info;
    if (uac_host_get_device_info(dev, &info) == ESP_OK) {
        flatten(s_product, sizeof(s_product), info.iProduct);
        report(dev, &info);
    }

    /* Opened, not started. There is no format to start it in until a
     * track is playing, and a stream running with nothing written to it
     * is isochronous bandwidth spent on silence. */
    s_dev = dev;
    s_hw_volume = false;
    s_volume_dirty = true;      /* apply the UI's level to the new device */
    s_rate = 0;
    s_channels = 0;
    s_attach_us = esp_timer_get_time();
    s_xfer_errors = 0;
    xSemaphoreGive(s_lock);

    s_present = true;
    s_generation++;
}

static void handle_disconnect(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_dev) {
        uac_host_device_close(s_dev);
        s_dev = NULL;
        s_rate = 0;
        s_channels = 0;
        ESP_LOGI(TAG, "USB audio output removed");
    }
    xSemaphoreGive(s_lock);
    s_product[0] = '\0';
}

static void uac_task(void *arg)
{
    (void)arg;
    uac_queue_msg_t msg;
    while (1) {
        /*
         * A timeout rather than portMAX_DELAY, because this task now
         * owns the volume control transfer as well as the plug events.
         *
         * Polling a flag at 20 Hz rather than queueing each change: only
         * the latest value matters, a drag emits one per UI poll, and a
         * four-deep queue would overflow within a fifth of a second of
         * dragging. Coalescing is the correct behaviour here and falls
         * out of the flag for free.
         */
        if (xQueueReceive(s_queue, &msg, pdMS_TO_TICKS(50)) != pdTRUE) {
            if (s_volume_dirty) apply_volume();
            continue;
        }
        if (s_volume_dirty) apply_volume();

        switch (msg.kind) {
        case MSG_ATTACH_TX:
            handle_connect(msg.addr, msg.iface_num);
            break;

        case MSG_DETACH:
            handle_disconnect();
            break;

        case MSG_ATTACH_RX:
            /* A microphone. Left closed on purpose: nothing in this
             * program reads audio in, and an open RX interface costs a
             * ring buffer and isochronous bandwidth for a stream that
             * would only be discarded. The example opened it because it
             * was looping mic to speaker; a music player is not. */
            ESP_LOGI(TAG, "input interface (itf %u) ignored", msg.iface_num);
            break;
        }
    }
}

/* ------------------------------------------------------------------ */

/* Called by usbhost.c on the bus task, after usb_host_install() and
 * before VBUS. */
static esp_err_t uac_class_install(void)
{
    const uac_host_driver_config_t cfg = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = tskNO_AFFINITY,
        .callback = driver_event_cb,
        .callback_arg = NULL,
    };
    return uac_host_install(&cfg);
}

esp_err_t uac_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    s_queue = xQueueCreate(EVENT_QUEUE_DEPTH, sizeof(uac_queue_msg_t));
    if (!s_queue) return ESP_ERR_NO_MEM;

    /* Above the UI at 4 and below the audio writer at 6. It runs twice
     * per plug event and holds the lock the writer needs, so it should
     * not be made to wait behind a redraw. */
    if (xTaskCreate(uac_task, "uac_events", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    return usbhost_register_class("uac", uac_class_install);
}
