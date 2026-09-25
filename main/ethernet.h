/*
 * ethernet.h -- a USB Ethernet adapter in the USB-A port.
 *
 * ASIX AX88772, AX88772A and AX88772B, through esp_usbh_asix, which is
 * an iot_eth driver. It is one more class driver on the bus usbhost.c
 * owns, registered the same way uac and hid are, with
 * skip_init_usb_host_driver set so it never installs or tears down the
 * stack underneath the others.
 *
 * It is always registered and needs no setting. With nothing plugged in
 * it is one usb_host client and two parked tasks; an adapter can arrive
 * before or after boot, and can be pulled and replugged.
 *
 * NOT A REPLACEMENT FOR THE RADIO, A SECOND ROUTE. Wi-Fi is untouched:
 * its switch, its saved networks and the portal all behave as before.
 * When the cable is up it is the default route (see netlink.h for why
 * that needs saying), and when it goes the default falls back to the
 * station if the station is joined.
 *
 * WHAT IT DOES NOT DO
 *
 * No NTP. sntp_start() is wifi.c's and is started from the station's
 * GOT_IP; a player on the cable alone keeps whatever time it had.
 * No hub-port power switching and no second adapter: one netif, bound
 * to the first ASIX device enumerated.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register the class driver with usbhost. Before usbhost_start(), like
 * uac_init() and hid_init(). Nothing touches the bus until then. */
esp_err_t ethernet_init(void);

/* Cable up and an address on it. See netlink.h for why both. */
bool ethernet_connected(void);

/* The cable's address as text, false if there is none. */
bool ethernet_ip(char *out, size_t out_size);

/* Either route. What netstream and the directory wait on. */
bool net_online(void);

/*
 * Which interface a connection opened now would leave by, as text:
 * "cable (asix) 192.168.1.124", "Wi-Fi 192.168.5.62", or "no route".
 * That is lwIP's default netif, which is how every socket here picks its
 * source: esp_http_client has no way to ask a socket where it went, but
 * a socket opened with no bind takes the default's address, so the
 * default at connect time IS the connection's route. At least 48 bytes.
 */
void net_route_describe(char *out, size_t out_size);

/*
 * One ICMP echo to the default route's gateway, and one to its DNS
 * server when that is a different address -- 5028.
 *
 * net_online() says an interface holds an address. That is not the same
 * as packets going anywhere: the board has shown a "connected" Wi-Fi
 * link whose coprocessor had stopped moving traffic, where every lookup
 * then ran its full 14 s before failing. A gateway that answers is the
 * cheapest proof the link is actually up.
 *
 * Returns true when the gateway answered. `gw_ms` and `dns_ms` are round
 * trips in milliseconds, or -1 for no answer (and -1 for DNS when it is
 * the gateway, or unset). `what`, at least 64 bytes, gets a one-line
 * account for the log. Blocks the caller for at most two timeouts.
 */
bool net_probe(uint32_t timeout_ms, int *gw_ms, int *dns_ms,
               char *what, size_t what_size);

#ifdef __cplusplus
}
#endif
