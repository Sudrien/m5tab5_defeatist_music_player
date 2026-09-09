/*
 * wifi.h -- the radio, which is on another chip.
 *
 * The P4 has no radio. The Tab5 carries an ESP32-C6-MINI-1U beside it,
 * reached over SDIO2, running slave firmware that speaks ESP-Hosted; the
 * `esp_wifi_remote` component makes that look like an ordinary esp_wifi
 * on this side. Everything below is about getting the C6 to exist.
 *
 * This is the spike, not the feature. It powers the module, releases it,
 * brings the transport up, scans, and logs what it heard. It does not
 * join anything, and there is nothing here for wifistore to talk to yet.
 * The point is to answer one question that cannot be answered on a host:
 * does the radio come up on this board under plain ESP-IDF.
 *
 * TWO GATES, IN ORDER, AND NEITHER IS OPTIONAL
 *
 * WLAN_3.3V is switched. U38 is a load switch from SOC_3.3V, enabled by
 * WLAN_PWR_EN, which is P0 of the PI4IOE5V6408 at 0x44 -- the second
 * expander, the one usbhost.c already drives for USB5V_EN on P3. Until
 * that bit is written the C6 is not merely uninitialised, it is
 * unpowered, and SDIO finds nothing on the other end.
 *
 Then RF_C6_RST, the C6's EN pin from GPIO15 (SOC_EXTRF_RST) through 1K
 * against a 10K pulldown -- but that one is esp_hosted's to drive, as
 * part of bringing the transport up. This file does the power only; two
 * owners of one reset pin would be decided by whichever ran second.
 *
 * The failure when the power is missed looks exactly like a host-side
 * fault -- sdmmc_card_init failing over and over while every pin on this
 * side logs correctly -- which is a long way to go to find an unwritten
 * expander bit.
 *
 * THE PINS ARE SET HERE, NOT IN A MENU
 *
 esp_hosted >= 3.0.2 carries per-board GPIO defaults and the Tab5 is
 * among them, but this file sets the pins itself with
 * esp_hosted_sdio_set_config() before esp_hosted_init(). A board'''s
 * wiring is not a build option: it cannot be chosen wrongly by anyone
 * holding this hardware, and a project that asks someone to pick it in a
 * menu has invented a way to get it wrong. Three builds went out with
 * the P4-Function-EV-Board pins because that menu had not been visited.
 *
 * PI4IOE2_IO_DIR is already 0xB9 and bit 0 is set, so P0 has been
 * configured as an output since the first boot of this program. Nothing
 * has ever driven it.
 *
 * THE ANTENNA IS ALREADY RIGHT
 *
 * RF_PTH_L_INT_H_EXT selects between the on-board 3D antenna and the
 * external MMCX through U29. It is P0 of expander 1, and PI4IOE1_OUT_SET
 * is 0x76 -- bit 0 clear, which is the internal antenna. Nothing here
 * touches it; it is noted so the next person does not go looking.
 *
 * SDIO2 IS NOT THE CARD
 *
 * The card is SDIO1 on GPIO39-44. The C6 is SDIO2 on GPIO8-13. Separate
 * pins and a separate slot, so storage_io.c's leases are unaffected and
 * the decode loop's read path is untouched by any of this.
 *
 * They do share the SDMMC host driver, which logs
 * "SDMMC host already initialized, skipping init flow" when the card got
 * there first. Harmless, and it is why this runs after storage_init().
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
 * Power the C6, bring the transport up, and scan once.
 *
 * `exp2` is the expander at 0x44, the same handle usbhost_init() takes.
 *
 Returns without doing anything, and without an error, when
 * settings_wifi_enabled() is false. That is the whole point of the
 * switch: with it off nothing here powers the module, initialises
 * ESP-Hosted or starts a driver, so "does this still behave like
 * v0.3.0" stays a one-bit question rather than an audit.
 *
 CALL IT FROM THE SETTINGS PUSH, NOT FROM app_main().
 *
 * settings_init() starts the writer task and loads nothing: which volume
 * the settings live on is not known until a volume turns up, so the file
 * is read by the first settings_note_path() that adopts one. Its own
 * header says the caller then has to push the values wherever they are
 * acted on, because nothing reads them by itself -- and the radio is one
 * of those values, beside the volume.
 *
 * Called from app_main() instead, this read the built-in default and
 * nothing else, forever: on the board that was `wifi=true` in the file at
 * 1960 ms and the file being read at 2232.
 *
 * Idempotent, because that push runs at the start of every track.
 *
 * Not ESP_ERROR_CHECK material at the call site: a player that will not
 * boot because its radio did not is worse than one that plays the card
 * in silence. Every failure path here logs and returns.
 */
esp_err_t wifi_probe(i2c_master_dev_handle_t exp2);

#ifdef __cplusplus
}
#endif
