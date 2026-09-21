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
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>

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

    /* The wire has to outrank the station, or plugging in does nothing
     * while Wi-Fi is joined. */
    CHECK(NETLINK_ETH_ROUTE_PRIO > NETLINK_WIFI_STA_ROUTE_PRIO,
          "cable route_prio %d does not beat the station's %d",
          NETLINK_ETH_ROUTE_PRIO, NETLINK_WIFI_STA_ROUTE_PRIO);

    printf("%s: %d checks, %d failures\n",
           failures ? "FAILURES" : "all passed", checks, failures);
    return failures ? 1 : 0;
}
