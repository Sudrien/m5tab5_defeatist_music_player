/*
 * storage.h -- the microSD slot and the USB-A port, mounted at the same
 * time and both hotpluggable.
 *
 * Two volumes rather than one "current" volume, because the chooser shows
 * a tab per slot and greys the empty one: something has to be able to say
 * "there is no card" without that meaning "there is nothing at all".
 *
 * Nothing here blocks. A poll task owns both mounts, and the rest of the
 * program only ever reads the flags.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STORAGE_SD = 0,
    STORAGE_USB = 1,
    STORAGE_COUNT
} storage_id_t;

#define STORAGE_SD_MOUNT    "/sd"
#define STORAGE_USB_MOUNT   "/usb"

/*
 * Mount the card if there is one, register the mass-storage class driver
 * with usbhost.c, and start the poll task.
 *
 * usbhost_init() must have run first: this registers a class driver with
 * it, and registration is refused once the port is up.
 *
 * Bus power is not this file's business any more. USB5V_EN, the host
 * stack and the library task live in usbhost.c, because there are two
 * class drivers on that port now and only one of them is looking for a
 * filesystem.
 *
 * Returns ESP_OK once the poll task is running. Neither volume is
 * necessarily mounted at that point; that is what storage_present() is
 * for.
 */
esp_err_t storage_init(void);

/*
 * Power the USB-A port and install the host stack.
 *
 * Idempotent, and asynchronous: the work happens on the poll task, not on
 * the caller's. It is three I2C writes and a 100 ms settle for the port's
 * inrush, and the two callers are the boot path and a touch event, neither
 * of which should block on it.
 *
 * Nothing calls this on its own. The port comes up only when there is a
 * reason:
 *
 *   1. no card at boot -- a player with nothing to play should look on
 *      the other port rather than sit there empty, and
 *   2. the USB tab is selected in the chooser, which is the user saying
 *      the same thing by hand.
 *
 * Deliberately one-way. Cutting VBUS again would yank a mounted drive out
 * from under whatever is reading it, and the saving is a port with
 * nothing plugged into it -- which is the state it was left in.
 */
void storage_usb_enable(void);

/* Has the port been asked for? True from the moment of the request, not
 * from the moment the poll task acts on it -- see the note on the
 * definition. Distinguishes "no drive" from "no power", which is the
 * difference between waiting and tapping. */
bool storage_usb_powered(void);

/* Mounted and readable right now. */
bool storage_present(storage_id_t id);

/* "/sd", "/usb". Valid whether or not the volume is mounted. */
const char *storage_mount_path(storage_id_t id);

/* "microSD", "USB" -- the tab labels. */
const char *storage_label(storage_id_t id);

/* Which volume a path lives on, or STORAGE_COUNT for neither. */
storage_id_t storage_of_path(const char *path);

/*
 * dir + "/" + name, without doubling the separator on a mount root
 * ("/sd" + "/" + "x", not "/sd//x"). FatFs tolerates the double slash and
 * it looks like a bug in every log line that carries it.
 *
 * False when the result would not fit, leaving out empty. Callers have to
 * check: a truncated path is a path to a different file, or to none, and
 * silently opening the wrong one is worse than not opening it.
 *
 * Written out rather than left as snprintf("%s%s%s") because the source
 * and destination buffers are the same size, so the compiler cannot prove
 * the concatenation fits and -Wformat-truncation is right to say so. The
 * arithmetic here is the proof.
 */
bool storage_join_path(char *out, size_t out_len, const char *dir, const char *name);

/* Bumped on every mount and every unmount. The chooser redraws its tabs
 * when this changes rather than re-statting the volumes every frame. */
uint32_t storage_generation(void);

/*
 * Tell the poll task that a file on this volume is open.
 *
 * Removal is detected by polling, and unmounting a volume with a FILE*
 * open on it is how you get a use-after-free inside FatFs rather than an
 * error return. So a held volume is marked absent -- the tab greys, the
 * player sees its track's volume vanish and stops -- and the unmount
 * itself waits for the release.
 *
 * STORAGE_COUNT releases without taking anything.
 */
void storage_hold(storage_id_t id);

#ifdef __cplusplus
}
#endif
