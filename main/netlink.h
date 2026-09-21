/*
 * netlink.h -- when the wired link counts as a network.
 *
 * Split out of ethernet.c for favmatch.h's reason: ethernet.c is event
 * handlers, a USB class driver and esp_netif, and can only be tested by
 * flashing. The decision it makes -- is there a network on the cable
 * right now -- is four booleans, and a host can test it completely.
 *
 * THE RULE: LINK AND ADDRESS, BOTH, AND THE LINK GOING TAKES THE ADDRESS
 *
 * The obvious gate is "have we been given an address", which is what
 * wifi_connected() is. On the cable it is wrong for two minutes at a
 * time. esp_netif does not post IP_EVENT_ETH_LOST_IP when the cable is
 * pulled; it starts a timer (CONFIG_ESP_NETIF_IP_LOST_TIMER_INTERVAL,
 * 120 s by default) and posts it when that runs out. An address-only
 * gate therefore reads "online" for two minutes after the cable is out,
 * and netstream spends them retrying a connection that has nowhere to
 * go instead of waiting for Wi-Fi, which may well be up.
 *
 * So the link state is the one that moves first. Link down clears the
 * address as well: a replug runs DHCP again and posts a fresh GOT_IP,
 * and until it does the address from before the unplug is not known to
 * be good -- it may be a different network on the other end.
 *
 * WIRED IS PREFERRED, AND THE CHOICE IS MADE HERE, NOT BY esp_netif
 *
 * Both interfaces can be up at once, and a socket leaves by the default
 * route. esp_netif picks the default itself, by route_prio, at every
 * link up and every address -- IPv4 or IPv6 -- and it picks between
 * interfaces that are UP, not interfaces that can route. That was tried
 * twice and lost twice on the board:
 *
 *   1000 gave the cable 200 over the station's 100. A replug made the
 *   cable the default the moment it linked, nine seconds before DHCP
 *   answered: "(default now no route)", Wi-Fi joined throughout.
 *
 *   1008 held it at 50 through the link-up pick and raised it after.
 *   The link-local IPv6 address arrives a second after the link, and
 *   adding it re-picks as well -- by then at 200. A stream reconnected
 *   in that window came up "via no route".
 *
 * So the cable's route_prio stays under the station's for good, and
 * esp_netif's own picking never chooses it. netlink_pick() below
 * decides, and ethernet.c applies the answer with
 * esp_netif_set_default_netif() at the three moments it can change: the
 * cable becoming usable, the cable stopping being usable, and the
 * station getting an address. That call pins the default -- esp_netif
 * stops re-picking until the pinned interface is destroyed -- which is
 * exactly the point: an IPv6 address turning up is no longer a reason
 * to route somewhere else.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Under ESP_NETIF_INHERENT_DEFAULT_WIFI_STA()'s 100, permanently: see
 * above. esp_netif on its own never makes the cable the default. */
#define NETLINK_ETH_ROUTE_PRIO      (50)
#define NETLINK_WIFI_STA_ROUTE_PRIO (100)

typedef enum {
    NETLINK_PICK_NONE,      /* leave esp_netif's choice alone */
    NETLINK_PICK_CABLE,
    NETLINK_PICK_STATION,
} netlink_pick_t;

/*
 * Which interface should be the default. A usable cable, always; else
 * the station, if there is one to pin. NONE when there is no station
 * netif at all (Wi-Fi switched off), because pinning a cable that has
 * just gone would stop esp_netif choosing the station when the radio is
 * turned back on -- the pin only lets go when its interface is
 * destroyed, and a cable's netif never is.
 */
static inline netlink_pick_t netlink_pick(bool cable_usable, bool station_exists)
{
    if (cable_usable) return NETLINK_PICK_CABLE;
    if (station_exists) return NETLINK_PICK_STATION;
    return NETLINK_PICK_NONE;
}


typedef enum {
    NETLINK_EV_LINK_UP,     /* IOT_ETH_EVENT_CONNECTED    */
    NETLINK_EV_LINK_DOWN,   /* IOT_ETH_EVENT_DISCONNECTED */
    NETLINK_EV_GOT_IP,      /* IP_EVENT_ETH_GOT_IP        */
    NETLINK_EV_LOST_IP,     /* IP_EVENT_ETH_LOST_IP       */
    NETLINK_EV_STOP,        /* IOT_ETH_EVENT_STOP         */
} netlink_ev_t;

typedef struct {
    bool link;
    bool addr;
} netlink_eth_t;

static inline bool netlink_eth_usable(const netlink_eth_t *s)
{
    return s && s->link && s->addr;
}

/*
 * Apply one event. Returns true when this event is the one that made the
 * cable usable -- the edge, not the level -- which is when ethernet.c
 * logs the address and kicks anything that was waiting for a network.
 */
static inline bool netlink_eth_step(netlink_eth_t *s, netlink_ev_t ev)
{
    if (!s) return false;
    const bool was = netlink_eth_usable(s);

    switch (ev) {
    case NETLINK_EV_LINK_UP:
        s->link = true;
        break;
    case NETLINK_EV_LINK_DOWN:
    case NETLINK_EV_STOP:
        s->link = false;
        s->addr = false;
        break;
    case NETLINK_EV_GOT_IP:
        s->addr = true;
        break;
    case NETLINK_EV_LOST_IP:
        s->addr = false;
        break;
    }

    return !was && netlink_eth_usable(s);
}

/* Anything that wants "is there a network", as opposed to "is the radio
 * joined". */
static inline bool netlink_online(bool wifi_connected, const netlink_eth_t *eth)
{
    return wifi_connected || netlink_eth_usable(eth);
}

#ifdef __cplusplus
}
#endif
