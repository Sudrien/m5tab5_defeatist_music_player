/*
 * ethernet.c -- see ethernet.h. The wired route through a USB adapter.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"

#include "esp_usbh_asix.h"
#include "iot_eth.h"
#include "iot_eth_netif_glue.h"

#include "ethernet.h"
#include "netlink.h"
#include "streamprobe.h"
#include "usbhost.h"
#include "wifi.h"

static const char *TAG = "tab5_eth";

static esp_netif_t *s_netif;

/*
 * Written only on the default event task, by on_event(); read from
 * anywhere. Two bools, and a reader that sees one updated and not the
 * other gets an answer that was true a moment ago, which is all a
 * "wait for the network" loop needs.
 */
static volatile netlink_eth_t s_state;

bool ethernet_connected(void)
{
    const netlink_eth_t s = s_state;
    return netlink_eth_usable(&s);
}

bool net_online(void)
{
    const netlink_eth_t s = s_state;
    return netlink_online(wifi_connected(), &s);
}

bool ethernet_ip(char *out, size_t out_size)
{
    if (!out || !out_size) return false;
    out[0] = '\0';
    if (!s_netif || !ethernet_connected()) return false;

    esp_netif_ip_info_t ip = { 0 };
    if (esp_netif_get_ip_info(s_netif, &ip) != ESP_OK) return false;
    if (ip.ip.addr == 0) return false;

    const int n = snprintf(out, out_size, IPSTR, IP2STR(&ip.ip));
    if (n <= 0 || (size_t)n >= out_size) {
        out[0] = '\0';
        return false;
    }
    return true;
}

/*
 * The default event task's stack is small and shared with wifi.c's
 * handler, so this only records and logs -- the same rule wifi.c's
 * on_event() follows.
 */
static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    netlink_ev_t ev;

    if (base == IOT_ETH_EVENT) {
        switch (id) {
        case IOT_ETH_EVENT_CONNECTED:    ev = NETLINK_EV_LINK_UP;   break;
        case IOT_ETH_EVENT_DISCONNECTED: ev = NETLINK_EV_LINK_DOWN; break;
        case IOT_ETH_EVENT_STOP:         ev = NETLINK_EV_STOP;      break;
        default: return;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_ETH_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        /* One Ethernet netif in this program, but the event is global:
         * anything else that ever posts ETH_GOT_IP is not this cable. */
        if (e && e->esp_netif != s_netif) return;
        if (e) ESP_LOGI(TAG, "address " IPSTR ", gateway " IPSTR,
                        IP2STR(&e->ip_info.ip), IP2STR(&e->ip_info.gw));
        ev = NETLINK_EV_GOT_IP;
    } else if (base == IP_EVENT && id == IP_EVENT_ETH_LOST_IP) {
        ev = NETLINK_EV_LOST_IP;
    } else {
        return;
    }

    netlink_eth_t s = s_state;
    const bool rose = netlink_eth_step(&s, ev);
    const bool was = ethernet_connected();
    s_state = s;

    if (ev == NETLINK_EV_LINK_UP)   ESP_LOGI(TAG, "cable connected");
    if (ev == NETLINK_EV_LINK_DOWN) ESP_LOGI(TAG, "cable disconnected");

    if (rose) {
        ESP_LOGI(TAG, "wired network up; it is the default route");
        streamprobe_kick();         /* once per boot; see streamprobe.h */
    } else if (was && !netlink_eth_usable(&s)) {
        ESP_LOGI(TAG, "wired network down%s",
                 wifi_connected() ? "; falling back to Wi-Fi" : "");
    }
}

/*
 * The class install, on the usbhost bus task, after usb_host_install()
 * and before VBUS -- which is what lets an adapter already in the port
 * enumerate against a client that exists.
 *
 * iot_eth_install() is where esp_usbh_asix registers its usb_host
 * client, so it has to happen here and not in ethernet_init().
 *
 * NOTHING ON THIS STACK. usbhost's task is 4096 bytes; everything here
 * is a handle, and the netif config is the few dozen bytes of an
 * inherent config.
 */
static esp_err_t eth_class_install(void)
{
    /* Process-wide and created once; wifi.c says why neither is undone.
     * The radio may or may not have got here first. */
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    ESP_RETURN_ON_ERROR(esp_event_handler_register(IOT_ETH_EVENT, ESP_EVENT_ANY_ID,
                                                   on_event, NULL),
                        TAG, "IOT_ETH_EVENT handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                                   on_event, NULL),
                        TAG, "ETH_GOT_IP handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP,
                                                   on_event, NULL),
                        TAG, "ETH_LOST_IP handler");

    /* usbhost.c owns the stack. Installing it a second time here would
     * fail, and uninstalling it on a driver error would take the drive
     * and the headset with it. */
    const esp_usbh_asix_config_t asix_cfg = {
        .skip_init_usb_host_driver = true,
        .task_coreid = -1,
    };
    iot_eth_driver_t *driver = NULL;
    ESP_RETURN_ON_ERROR(esp_usbh_asix_new_eth(&asix_cfg, &driver), TAG, "asix driver");

    const iot_eth_config_t eth_cfg = {
        .driver = driver,
        .stack_input = NULL,
    };
    iot_eth_handle_t eth = NULL;
    ESP_RETURN_ON_ERROR(iot_eth_install(&eth_cfg, &eth), TAG, "iot_eth_install");

    esp_netif_inherent_config_t inherent = ESP_NETIF_INHERENT_DEFAULT_ETH();
    inherent.if_key = "USB_ASIX";
    inherent.if_desc = "usb eth";
    /* See netlink.h: the stock 50 loses to the station's 100, and a
     * cable nobody is using is not what plugging one in asks for. */
    inherent.route_prio = NETLINK_ETH_ROUTE_PRIO;
    const esp_netif_config_t netif_cfg = {
        .base = &inherent,
        .driver = NULL,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };
    s_netif = esp_netif_new(&netif_cfg);
    if (!s_netif) return ESP_ERR_NO_MEM;

    iot_eth_netif_glue_handle_t glue = iot_eth_new_netif_glue(eth);
    if (!glue) return ESP_ERR_NO_MEM;
    ESP_RETURN_ON_ERROR(esp_netif_attach(s_netif, glue), TAG, "netif attach");

    ESP_RETURN_ON_ERROR(iot_eth_start(eth), TAG, "iot_eth_start");
    ESP_LOGI(TAG, "USB Ethernet ready (ASIX AX88772/A/B)");
    return ESP_OK;
}

esp_err_t ethernet_init(void)
{
    return usbhost_register_class("eth", eth_class_install);
}
