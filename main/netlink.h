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
 * WIRED IS PREFERRED, AND THAT IS lwIP's DECISION, NOT THIS FILE'S
 *
 * Both interfaces can be up at once. Which one a socket uses is the
 * default route, and esp_netif picks the default as the up interface
 * with the highest route_prio. The Wi-Fi station is 100; the stock
 * Ethernet inherent config is 50, which would leave a plugged-in cable
 * idle behind the radio. NETLINK_ETH_ROUTE_PRIO is what ethernet.c
 * sets instead.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Above ESP_NETIF_INHERENT_DEFAULT_WIFI_STA()'s 100, so a cable wins. */
#define NETLINK_ETH_ROUTE_PRIO      (200)
#define NETLINK_WIFI_STA_ROUTE_PRIO (100)

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
