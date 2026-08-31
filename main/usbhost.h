/*
 * usbhost.h -- one owner for the USB-A port.
 *
 * There are two class drivers on this bus now, mass storage and audio,
 * and exactly one host stack and one VBUS enable underneath them. Before
 * this file, storage.c owned all three, which was correct while it was
 * the only client and became wrong the moment a second one appeared: a
 * headset plugged into a player with a card in the slot would have found
 * the port dark, because the only thing that ever asked for power was
 * the thing looking for a filesystem.
 *
 * So the bus is its own subsystem. Class drivers register themselves
 * before the port comes up and are installed in registration order,
 * between the host stack and VBUS -- which is the ordering storage.c
 * already documented and which matters for the same reason it did then:
 * a device already in the port enumerates the instant power arrives, and
 * it should meet a stack that exists.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Install a class driver. Called from the class driver's own start
 * function, on the bus task, after usb_host_install() and before VBUS.
 * Returning anything but ESP_OK is logged and the remaining classes are
 * installed anyway -- one class failing should not take the port down
 * for the other.
 */
typedef esp_err_t (*usbhost_class_fn)(void);

/*
 * Register a class driver. Safe before or after usbhost_init(), and it
 * must happen before usbhost_start(): classes registered after the port
 * is up are rejected rather than installed late, because a class driver
 * that misses the enumeration of a device already attached will not see
 * that device again until it is unplugged.
 */
esp_err_t usbhost_register_class(const char *name, usbhost_class_fn fn);

/*
 * Record the expander and start the bus task. Powers nothing.
 *
 * exp2 is the PI4IOE5V6416 at 0x44. USB5V_EN is P3 of that expander, not
 * a GPIO: until it is driven high the port is electrically dead -- the
 * host stack installs, every class driver registers, and nothing ever
 * enumerates, with no error anywhere to say why.
 */
esp_err_t usbhost_init(i2c_master_dev_handle_t exp2);

/*
 * Bring the port up: host stack, class drivers, then VBUS.
 *
 * Idempotent and asynchronous. The work is three I2C writes and a 100 ms
 * settle for the port's inrush, so it happens on the bus task and never
 * on the caller's.
 */
void usbhost_start(void);

/*
 * Turn bus power on or off.
 *
 * THIS USED TO BE REFUSED, and the reason it was refused is still true:
 * cutting VBUS yanks a mounted drive or a playing headset out from under
 * whatever is using it. What has changed is that there is now somewhere
 * to ask for it from -- the settings panel -- and therefore someone to
 * hold responsible for the ordering. The caller must have got the
 * filesystem out of the way first; storage_usb_power() is the one that
 * knows how, and is what the panel actually calls.
 *
 * The host stack is NOT torn down. Only the P3 line moves. Uninstalling
 * and reinstalling the USB stack and its class drivers is a great deal
 * of state to rebuild for no gain, and class drivers cannot re-register
 * once the port has been up. Powering back on is therefore just VBUS
 * again, and a device plugged in meanwhile enumerates against the stack
 * that has been sitting there all along.
 *
 * Asynchronous, like usbhost_start(), and for the same reason: the
 * caller is the UI task and the work is I2C plus a settle.
 */
void usbhost_set_power(bool on);

/* Whether VBUS is being driven right now. Distinct from
 * usbhost_powered(), which is about the port ever having been asked
 * for. */
bool usbhost_vbus_on(void);

/* True from the moment the port has been *asked* for, not from the
 * moment the bus task acts on it. Callers that draw a label off this
 * want the request: the task can be a moment behind, and a label that
 * flickers back to "off" mid-bring-up reads as the request having been
 * lost. */
bool usbhost_powered(void);

/* True once the stack is installed and VBUS is actually high. */
bool usbhost_running(void);

#ifdef __cplusplus
}
#endif
