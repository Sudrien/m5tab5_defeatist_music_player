/*
 * battery.h -- state of charge, as a number the UI can draw.
 *
 * The Tab5 measures its own pack with an INA226 (U31 on the schematic)
 * on the same I2C bus everything else here is on: a bus voltage register
 * and a shunt voltage register, 16 bits each. That is the whole of the
 * hardware interface, and it is why this file is small.
 *
 * The pack is an NP-F550: **2S, nominally 7.4 V**, full around 8.2 V and
 * flat around 6.0 V. Not a single cell. Every curve in this file is for
 * two in series, and a single-cell curve here does not read low -- it
 * pins at 100% for the entire discharge, because 7 V is off the top of
 * it. That is the failure worth naming, since it looks like a working
 * gauge on a full battery and stays looking like one.
 *
 * WHAT IS HONEST AND WHAT IS NOT
 *
 * The voltage is measured. The percentage is not -- it is that voltage
 * put through a piecewise curve for two lithium cells in series, which
 * is a guess, and a worse one under load than at rest. It moves when the
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
 * Charging is the sign of the current through the shunt (R39, 5 mohm).
 * On this board **positive is discharging and negative is charging**,
 * which is the opposite of the intuitive reading and is why
 * BATTERY_CHARGE_SIGN exists rather than a bare comparison. Flip it if
 * the fill turns green exactly when it should not.
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
 * From the schematic rather than from folklore: U31's A1 pin is tied to
 * GND and A0 to SOC_3.3V, which is 0x41 in the INA226 address table.
 * SCL/SDA are xG32_SYS_SCL / xG31_SYS_SDA -- the same bus as the two
 * PI4IOE expanders and the ES8388, so it takes the bus handle this file
 * is handed rather than making its own.
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
/*
 * DIAGNOSTIC, added by 0505. Not a feature.
 *
 * Capture pack voltage as fast as the INA226 will produce it, for a
 * short window, and dump the result. Armed at a seek commit, which is
 * where the cyan flash lands.
 *
 * Everything else in this file is built to hide what this is looking
 * for. The part averages 16 samples, the conversions are 1.1 ms, the
 * poll is every 5 s and the result goes through a 1/8 exponential
 * average -- four separate defences against the amplifier's draw
 * showing up in the gauge, all of them correct for a gauge and all of
 * them fatal to a transient. So the trace reconfigures the part for
 * 140 us bus-only conversions with no averaging, samples flat out into
 * a buffer, restores the normal configuration and only then logs.
 *
 * Why it matters: a bus-powered keyboard browned the board out on
 * every plug-in, which is direct evidence that a load step on this
 * supply can reach the SoC. A step too small to trip the brownout
 * detector is the only remaining explanation for a single corrupted
 * DSI frame that leaves no underrun, no error and no log -- which is
 * what six patches of software theories have failed to account for.
 *
 * Costs the I2C bus for the length of the window, which the touch
 * controller and the codec are also on. That is acceptable for a
 * diagnostic and is why this is armed rather than free-running.
 *
 * Safe to call from anywhere, including when no gauge was found. Calls
 * while a trace is already running are ignored rather than queued.
 */
void battery_trace_arm(const char *why);

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
