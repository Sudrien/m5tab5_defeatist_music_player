/*
 * ethcfg.h -- which USB configuration to ask a Realtek adapter for.
 *
 * The RTL8152 and RTL8153 come up in configuration 1, which is
 * Realtek's own vendor protocol, and nothing on this side speaks it.
 * The same chips carry a standard CDC-ECM function as configuration 2,
 * and espressif/iot_usbh_ecm speaks that. ESP-IDF always enumerates
 * configuration 1 unless an enumeration filter says otherwise, so
 * usbhost.c installs one and this is its decision.
 *
 * Header-only so a host can test it: a wrong answer here does not
 * fail, it enumerates the adapter in a configuration nobody claims and
 * the port looks dead.
 *
 * ONLY WHEN THE DEVICE SAYS THERE IS A SECOND. A request for a
 * configuration the device does not have fails enumeration outright,
 * which is worse than leaving it in configuration 1.
 *
 * Every other device -- the drive, the headset, the ASIX adapter,
 * which has one configuration -- is left alone.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ETHCFG_REALTEK_VID  (0x0bda)
#define ETHCFG_ECM_CONFIG   (2)

/*
 * Returns the configuration to enumerate, or 0 to leave the stack's
 * choice alone.
 */
static inline uint8_t ethcfg_select(uint16_t vid, uint16_t pid, uint8_t num_configs)
{
    if (vid != ETHCFG_REALTEK_VID) return 0;
    if (pid != 0x8152 && pid != 0x8153) return 0;
    if (num_configs < ETHCFG_ECM_CONFIG) return 0;
    return ETHCFG_ECM_CONFIG;
}

#ifdef __cplusplus
}
#endif
