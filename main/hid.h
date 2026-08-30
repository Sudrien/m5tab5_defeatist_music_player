/*
 * hid.h -- the buttons on a USB headset's inline remote.
 *
 * A USB audio device is very often three interfaces: a speaker, a
 * microphone and a HID collection carrying volume and mute keys. uac.c
 * takes the first, ignores the second, and this file takes the third.
 *
 * It is not a class driver. There is a HID host component in the
 * registry, and it is not usable here for the reason the example this is
 * ported from ran into: plenty of composite devices -- the C-Media
 * headset chips are the well-known example -- answer the standard
 * descriptors and then stall EP0 for the class-specific report
 * descriptor. A driver built around parsing that descriptor has nothing
 * to work with. The interrupt IN endpoint still delivers reports.
 *
 * So this claims the interface through the plain host client API and
 * polls the endpoint. What the bits mean cannot be derived and has to be
 * a table -- see BUTTON_BITS in hid.c, and the note above it about what
 * to do when a button turns out to be the wrong one.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HID_BTN_VOL_UP = 0,
    HID_BTN_VOL_DOWN,
    HID_BTN_MUTE,
    HID_BTN_MIC_MUTE,

    /*
     * Transport. Added with the report-descriptor parsing in 0506: a
     * keyboard's media keys are Consumer usages and there was nothing
     * here to map them onto.
     */
    HID_BTN_PLAY_PAUSE,
    HID_BTN_NEXT,
    HID_BTN_PREV,
    HID_BTN_STOP,
} hid_button_t;

/*
 * Called on a press -- the transition to held, not on the repeats that
 * follow and not on release.
 *
 * Runs on the polling task, which is also the task that owns the URB, so
 * it must not block and must not do I/O. Publish a value and let the UI
 * task act on it; player.c does exactly that, and routes the result
 * through the same switch the on-screen buttons go through so both
 * sources log and behave identically.
 */
typedef void (*hid_button_cb_t)(hid_button_t button);

/*
 * Register the USB host client and start watching for HID interfaces.
 *
 * Must run before usbhost_start(), like the class drivers: registration
 * is refused once the port is up, and a client registered after
 * enumeration is not offered the devices already attached.
 */
esp_err_t hid_init(hid_button_cb_t cb);

#ifdef __cplusplus
}
#endif
