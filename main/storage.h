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

/* Has the port been asked for? True from the moment of the request
 * rather than from the moment the bus task acts on it. The port is now
 * powered unconditionally at boot, so this is only ever false for the
 * few milliseconds of bring-up -- but the chooser draws a label off it,
 * and a label that says "no port" for one frame is worse than one that
 * says "waiting for a drive" for one frame too many. */
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

/*
 * Should this directory entry be ignored entirely?
 *
 * True for "." and "..", for dotfiles, and therefore for the "._Name.mp3"
 * AppleDouble sidecars macOS leaves on every FAT volume it touches. Those
 * are the reason this is shared rather than a line in each scanner: they
 * end in .mp3, so decoder_supports() says yes, and they contain a
 * resource fork rather than audio -- a folder written on a Mac has one
 * per track and they interleave with the real files.
 *
 * The rule lived in browser.c only, so the chooser hid them and
 * "play folder" queued them. Two scanners disagreeing about what a file
 * is is the kind of thing that stays broken because each one looks
 * correct on its own.
 */
bool storage_is_hidden(const char *name);

/*
 * Set the FAT hidden attribute on a file this program wrote.
 *
 * The dot prefix storage_is_hidden() looks for means nothing to
 * Windows, so the settings file and the per-track sidecars show up as
 * clutter on the machine people are most likely to plug the card into.
 * This is the other half of hiding them.
 *
 * Call after the file is in place -- after the rename, not on the temp
 * file, since the attribute travels with the name. Advisory: failure is
 * silent and costs nothing but visibility.
 */
void storage_mark_hidden(const char *path);

/*
 * What is in the slot and what is on the port, for the settings panel.
 *
 * Everything here was already logged once at mount and then existed only
 * in the boot output -- which is the wrong place for it, because the
 * question "is this card slow, or is this file just badly encoded" is
 * asked in front of the player, hours later, by someone who is not
 * looking at a serial console.
 *
 * Read-only snapshots rather than a pointer to the live sdmmc_card_t or
 * msc device handle. Those belong to the poll task and are freed on
 * unmount; a panel holding one across a removal is the use-after-free
 * that storage_hold() exists to prevent, and there is no reason to hand
 * out the loaded gun when the panel only wants six numbers.
 *
 * present false means the rest is not meaningful. It is still zeroed
 * rather than left stale, so a drawn field cannot be last session's.
 */
typedef struct {
    bool     present;
    char     name[32];      /* CID product name, or "" */
    char     type[16];      /* "SDHC/SDXC", "SDSC", "MMC/eMMC" */
    uint64_t capacity_mb;
    int      speed_khz;     /* negotiated, not the card's rating */
    int      bus_width;     /* 1 or 4 */
} storage_sd_info_t;

typedef struct {
    bool     present;       /* a drive is mounted */
    bool     powered;       /* VBUS asked for */
    char     product[36];
    char     manufacturer[36];
    uint16_t vid;
    uint16_t pid;
    uint64_t capacity_mb;
    uint32_t sector_size;
} storage_usb_info_t;

/*
 * Switch USB bus power, with the filesystem got out of the way first.
 *
 * This is the call the settings panel makes, and the ordering is the
 * whole of it: cutting VBUS under a mounted volume is a physical unplug
 * as far as FatFs is concerned, and doing that with a FILE* open on it
 * is the use-after-free storage_hold() exists to prevent.
 *
 * So powering off unmounts first, and REFUSES while the volume is held
 * -- returns false and changes nothing, because the alternative is
 * stopping a track the listener did not ask to stop. Take the drive out
 * of the playlist first, or let the track end.
 *
 * Powering on is unconditional and asynchronous; the drive enumerates
 * when it enumerates.
 *
 * Returns whether the request was accepted, not whether the line has
 * moved -- the line moves on the bus task a moment later.
 */
bool storage_usb_power(bool on);

/* Is the USB volume the one a track is being read from? The panel greys
 * its switch on this, so a refusal is visible before it is a refusal. */
bool storage_usb_busy(void);

/* Both fill out completely, including the false case. Never fail. */
void storage_sd_info(storage_sd_info_t *out);
void storage_usb_info(storage_usb_info_t *out);

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
