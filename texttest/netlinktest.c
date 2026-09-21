/*
 * netlinktest.c -- when the USB Ethernet cable counts as a network, in
 * main/netlink.h, compiled from that header rather than transcribed.
 *
 * ethernet.c around it is event handlers, esp_usbh_asix and esp_netif,
 * and runs on the board only. What is tested here is what netstream and
 * the station directory end up waiting on.
 *
 * THE CASE WORTH THE FILE is the unplug. esp_netif posts ETH_LOST_IP
 * two minutes after the cable goes, not when it goes; a gate built on
 * the address alone reads "online" for those two minutes. The replays
 * below are in the order the board delivers them.
 *
 * The Realtek configuration choice in main/ethcfg.h is checked at the
 * end, for the same reason: it is the rule, and usbhost.c's filter is
 * one call to it.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>

#include "ethcfg.h"
#include "netlink.h"

static int checks, failures;

#define CHECK(cond, ...) do {                                   \
    checks++;                                                   \
    if (!(cond)) {                                              \
        failures++;                                             \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__);           \
        printf(__VA_ARGS__);                                    \
        printf("\n");                                           \
    }                                                           \
} while (0)

int main(void)
{
    netlink_eth_t s = { 0 };

    /* Boot, no adapter. */
    CHECK(!netlink_eth_usable(&s), "usable with nothing plugged in");
    CHECK(!netlink_online(false, &s), "online with no route at all");
    CHECK(netlink_online(true, &s), "Wi-Fi alone did not count");
    CHECK(!netlink_eth_usable(NULL), "NULL state usable");
    CHECK(!netlink_eth_step(NULL, NETLINK_EV_LINK_UP), "NULL state stepped");

    /* Plug in: link first, then DHCP. The edge is the address. */
    CHECK(!netlink_eth_step(&s, NETLINK_EV_LINK_UP), "link alone rose");
    CHECK(!netlink_eth_usable(&s), "usable before an address");
    CHECK(netlink_eth_step(&s, NETLINK_EV_GOT_IP), "address did not rise");
    CHECK(netlink_eth_usable(&s), "not usable with link and address");
    CHECK(netlink_online(false, &s), "cable alone did not count");

    /* A lease renewal posts GOT_IP again. Not a second edge: the kick
     * behind it is once per boot, but the log line is not. */
    CHECK(!netlink_eth_step(&s, NETLINK_EV_GOT_IP), "renewal rose again");
    CHECK(netlink_eth_usable(&s), "renewal dropped the link");

    /* Unplug. DISCONNECTED arrives at once; LOST_IP does not. The gate
     * must fall on the first. */
    CHECK(!netlink_eth_step(&s, NETLINK_EV_LINK_DOWN), "unplug rose");
    CHECK(!netlink_eth_usable(&s), "still usable after unplug -- the 120 s window");
    CHECK(!s.addr, "unplug kept the old address");
    CHECK(netlink_online(true, &s), "Wi-Fi did not take over");

    /* ...and two minutes later, the late LOST_IP is harmless. */
    CHECK(!netlink_eth_step(&s, NETLINK_EV_LOST_IP), "late LOST_IP rose");
    CHECK(!netlink_eth_usable(&s), "late LOST_IP made it usable");

    /* Replug. The old address does not come back with the link: the
     * other end may be a different network. */
    CHECK(!netlink_eth_step(&s, NETLINK_EV_LINK_UP), "replug rose without DHCP");
    CHECK(!netlink_eth_usable(&s), "replug reused the address from before");
    CHECK(netlink_eth_step(&s, NETLINK_EV_GOT_IP), "replug lease did not rise");

    /* Lease lost with the cable still in: address goes, link stays, and
     * the next lease is an edge again. */
    CHECK(!netlink_eth_step(&s, NETLINK_EV_LOST_IP), "LOST_IP rose");
    CHECK(!netlink_eth_usable(&s), "usable without an address");
    CHECK(s.link, "LOST_IP took the link");
    CHECK(netlink_eth_step(&s, NETLINK_EV_GOT_IP), "new lease did not rise");

    /* Driver stop clears both, like an unplug. */
    CHECK(!netlink_eth_step(&s, NETLINK_EV_STOP), "stop rose");
    CHECK(!s.link && !s.addr, "stop left state behind");

    /* GOT_IP before CONNECTED: the address is held but does not count
     * until the link says so, and the link is then the edge. */
    s = (netlink_eth_t){ 0 };
    CHECK(!netlink_eth_step(&s, NETLINK_EV_GOT_IP), "address without link rose");
    CHECK(!netlink_eth_usable(&s), "usable without link");
    CHECK(netlink_eth_step(&s, NETLINK_EV_LINK_UP), "link after address did not rise");

    /* esp_netif must never choose the cable by itself: it re-picks at
     * link up and at every IPv6 address, before the cable can route. */
    CHECK(NETLINK_ETH_ROUTE_PRIO < NETLINK_WIFI_STA_ROUTE_PRIO,
          "cable route_prio %d beats the station's %d -- esp_netif will pick it unaddressed",
          NETLINK_ETH_ROUTE_PRIO, NETLINK_WIFI_STA_ROUTE_PRIO);

    /* So ethernet.c picks, and pins. */
    CHECK(netlink_pick(true, true) == NETLINK_PICK_CABLE, "usable cable not preferred");
    CHECK(netlink_pick(true, false) == NETLINK_PICK_CABLE, "usable cable, no radio");
    CHECK(netlink_pick(false, true) == NETLINK_PICK_STATION,
          "cable gone and the station not pinned back");
    CHECK(netlink_pick(false, false) == NETLINK_PICK_NONE,
          "pinned something with no station to pin -- the radio could never be picked again");

    /* The replay that 1008 lost: link up, then the IPv6 link-local a
     * second later, then IPv4. Until the IPv4 lease the cable is not
     * usable, so the pick stays the station through all of it. */
    s = (netlink_eth_t){ 0 };
    netlink_eth_step(&s, NETLINK_EV_LINK_UP);
    CHECK(netlink_pick(netlink_eth_usable(&s), true) == NETLINK_PICK_STATION,
          "link up alone moved the default to the cable");
    /* (IPv6 is not an event here at all: it cannot make the cable usable.) */
    netlink_eth_step(&s, NETLINK_EV_GOT_IP);
    CHECK(netlink_pick(netlink_eth_usable(&s), true) == NETLINK_PICK_CABLE,
          "the IPv4 lease did not move the default to the cable");

    /* Which configuration a Realtek adapter is enumerated in (ethcfg.h).
     * Configuration 2 is CDC-ECM on the RTL8152 and RTL8153. */
    CHECK(ethcfg_select(0x0bda, 0x8153, 2) == 2, "RTL8153 not moved to ECM");
    CHECK(ethcfg_select(0x0bda, 0x8152, 2) == 2, "RTL8152 not moved to ECM");
    CHECK(ethcfg_select(0x0bda, 0x8153, 3) == 2, "three configs, ECM not chosen");

    /* A configuration the device does not have fails enumeration, which
     * is worse than leaving it alone. */
    CHECK(ethcfg_select(0x0bda, 0x8153, 1) == 0, "asked a one-config device for 2");
    CHECK(ethcfg_select(0x0bda, 0x8153, 0) == 0, "asked a zero-config device for 2");

    /* Everyone else keeps the stack's choice: the ASIX adapter, the
     * Realtek card readers and audio parts under the same vendor ID,
     * and anything that is not Realtek at all. */
    CHECK(ethcfg_select(0x0b95, 0x772b, 2) == 0, "ASIX touched");
    CHECK(ethcfg_select(0x0bda, 0x0129, 2) == 0, "Realtek card reader touched");
    CHECK(ethcfg_select(0x0bda, 0x4014, 2) == 0, "Realtek audio touched");
    CHECK(ethcfg_select(0x0781, 0x8153, 2) == 0, "matched a PID under another vendor");

    printf("%s: %d checks, %d failures\n",
           failures ? "FAILURES" : "all passed", checks, failures);
    return failures ? 1 : 0;
}
