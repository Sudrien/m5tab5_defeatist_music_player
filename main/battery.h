/*
 * battery.h -- state of charge, as a number the UI can draw.
 *
 * The Tab5 measures its own pack with an INA226 on the same I2C bus
 * everything else here is on: a bus voltage register and a shunt voltage
 * register, 16 bits each. That is the whole of the hardware interface,
 * and it is why this file is small.
 *
 * WHAT IS HONEST AND WHAT IS NOT
 *
 * The voltage is measured. The percentage is not -- it is that voltage
 * put through a piecewise curve for a single lithium cell, which is a
 * guess, and a worse one under load than at rest. It moves when the
 * amplifier is driving the speaker and moves back when headphones are
 * plugged in, and no amount of curve fitting fixes that; coulomb
 * counting would, and needs a charge reference this does not have.
 *
 * So: the percentage is rounded to 5 and the reading is smoothed, which
 * is not accuracy, it is a refusal to display precision that is not
 * there. A gauge that reads 63% and then 61% and then 64% while nothing
 * happened is worse than one that reads 60% and stays put, because the
 * movement is the part people believe.
 *
 * Charging is the sign of the current through the shunt, and the sign
 * depends on which way round the shunt is wired. BATTERY_CHARGE_SIGN is
 * the knob if the bolt appears exactly when it should not.
 *
 * THREADING
 *
 * One task writes, everyone reads, and what crosses is a pair of ints --
 * the same rule as s_ring_pct in player.c. There is no handle to share
 * and therefore nothing to dangle.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * INA226, 0x41 on the internal bus.
 *
 * If the probe at init logs a device that answers but reads a constant,
 * this address is the first thing to doubt -- it is strapped by two
 * address pins and 0x40, 0x44 and 0x45 are the other plausible ones.
 */
#define BATTERY_INA226_ADDR     (0x41)

/*
 * Add the device and start it converting. Returns ESP_ERR_NOT_FOUND when
 * nothing answers, which is not fatal: the gauge simply reads unknown
 * and the player is otherwise unaffected.
 */
esp_err_t battery_init(i2c_master_bus_handle_t bus);

/* Start the poll task. Safe to skip if battery_init() failed. */
esp_err_t battery_start(void);

/*
 * State of charge 0-100, or -1 when there is no reading yet and -1
 * forever when there is no gauge. The UI draws an empty outline with no
 * digits for -1 rather than inventing a number.
 */
int battery_pct(void);

/* Whether current is flowing into the pack. */
bool battery_charging(void);

/* Pack voltage in millivolts, or 0 when unknown. For logging -- the UI
 * shows the percentage, because a voltage is a number almost nobody can
 * convert into "will this last the album". */
int battery_mv(void);

#ifdef __cplusplus
}
#endif
