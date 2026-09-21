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
#include "iot_usbh_cdc.h"
#include "iot_usbh_ecm.h"
#include "iot_eth.h"
#include "iot_eth_netif_glue.h"

#include "ethernet.h"
#include "netlink.h"
#include "streamprobe.h"
#include "usbhost.h"
#include "wifi.h"

static const char *TAG = "tab5_eth";

/*
 * One per driver: the ASIX one and the CDC-ECM one each have their own
 * iot_eth handle and their own netif, because iot_eth binds one driver
 * to one handle. Both can be installed with nothing plugged in; which
 * one comes up is decided by what is in the port.
 *
 * state is written only on the default event task, by on_event(), and
 * read from anywhere. Two bools, and a reader that sees one updated and
 * not the other gets an answer that was true a moment ago, which is all
 * a "wait for the network" loop needs.
 */
typedef struct {
    const char       *name;
    iot_eth_handle_t  eth;
    esp_netif_t      *netif;
    volatile netlink_eth_t state;
} eth_if_t;

enum { IF_ASIX, IF_ECM, IF_COUNT };

static eth_if_t s_if[IF_COUNT] = {
    [IF_ASIX] = { .name = "asix" },
    [IF_ECM]  = { .name = "ecm"  },
};

static bool if_usable(const eth_if_t *i)
{
    const netlink_eth_t s = i->state;
    return netlink_eth_usable(&s);
}

/* The first usable interface, or NULL. */
static const eth_if_t *usable_if(void)
{
    for (int k = 0; k < IF_COUNT; k++) {
        if (if_usable(&s_if[k])) return &s_if[k];
    }
    return NULL;
}

bool ethernet_connected(void)
{
    return usable_if() != NULL;
}

bool net_online(void)
{
    return wifi_connected() || ethernet_connected();
}

void net_route_describe(char *out, size_t out_size)
{
    if (!out || !out_size) return;
    esp_netif_t *nif = esp_netif_get_default_netif();
    esp_netif_ip_info_t ip = { 0 };
    if (!nif || esp_netif_get_ip_info(nif, &ip) != ESP_OK || ip.ip.addr == 0) {
        snprintf(out, out_size, "no route");
        return;
    }
    const char *what = "Wi-Fi";
    for (int k = 0; k < IF_COUNT; k++) {
        if (nif == s_if[k].netif) what = s_if[k].name;
    }
    if (what[0] == 'W') {
        snprintf(out, out_size, "Wi-Fi " IPSTR, IP2STR(&ip.ip));
    } else {
        snprintf(out, out_size, "cable (%s) " IPSTR, what, IP2STR(&ip.ip));
    }
}

bool ethernet_ip(char *out, size_t out_size)
{
    if (!out || !out_size) return false;
    out[0] = '\0';
    const eth_if_t *i = usable_if();
    if (!i || !i->netif) return false;

    esp_netif_ip_info_t ip = { 0 };
    if (esp_netif_get_ip_info(i->netif, &ip) != ESP_OK) return false;
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
    eth_if_t *i = NULL;

    if (base == IOT_ETH_EVENT) {
        /* iot_eth posts a pointer to its handle, which is how the two
         * drivers' events are told apart. */
        const iot_eth_handle_t h = data ? *(const iot_eth_handle_t *)data : NULL;
        for (int k = 0; k < IF_COUNT; k++) {
            if (h && s_if[k].eth == h) i = &s_if[k];
        }
        if (!i) return;
        switch (id) {
        case IOT_ETH_EVENT_CONNECTED:
            /*
             * Down to PENDING before esp_netif sees the link, so its
             * link-up pick of the default keeps Wi-Fi. This handler runs
             * first because it was registered first: IOT_ETH_EVENT
             * handlers for ANY id run in registration order, and the
             * netif glue's -- the one that brings the interface up and
             * re-picks the default -- is registered at attach(), after
             * this. on_link_up_after() puts it back. See netlink.h.
             */
            if (i->netif) esp_netif_set_route_prio(i->netif, NETLINK_ETH_ROUTE_PRIO_PENDING);
            ev = NETLINK_EV_LINK_UP;
            break;
        case IOT_ETH_EVENT_DISCONNECTED: ev = NETLINK_EV_LINK_DOWN; break;
        case IOT_ETH_EVENT_STOP:         ev = NETLINK_EV_STOP;      break;
        default: return;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_ETH_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        for (int k = 0; k < IF_COUNT; k++) {
            if (e && e->esp_netif == s_if[k].netif) i = &s_if[k];
        }
        if (!i) return;
        ESP_LOGI(TAG, "%s: address " IPSTR ", gateway " IPSTR, i->name,
                 IP2STR(&e->ip_info.ip), IP2STR(&e->ip_info.gw));
        ev = NETLINK_EV_GOT_IP;
    } else if (base == IP_EVENT && id == IP_EVENT_ETH_LOST_IP) {
        const ip_event_got_ip_t *e = data;
        for (int k = 0; k < IF_COUNT; k++) {
            if (e && e->esp_netif == s_if[k].netif) i = &s_if[k];
        }
        if (!i) return;
        ev = NETLINK_EV_LOST_IP;
    } else {
        return;
    }

    netlink_eth_t s = i->state;
    const bool was = netlink_eth_usable(&s);
    const bool rose = netlink_eth_step(&s, ev);
    i->state = s;

    if (ev == NETLINK_EV_LINK_UP)   ESP_LOGI(TAG, "%s: cable connected", i->name);
    if (ev == NETLINK_EV_LINK_DOWN) ESP_LOGI(TAG, "%s: cable disconnected", i->name);

    if (rose) {
        ESP_LOGI(TAG, "%s: wired network up; it is the default route", i->name);
        streamprobe_kick();         /* once per boot; see streamprobe.h */
    } else if (was && !netlink_eth_usable(&s)) {
        ESP_LOGI(TAG, "%s: wired network down%s", i->name,
                 wifi_connected() ? "; falling back to Wi-Fi" : "");
    }
}

/*
 * After the glue's link-up handling, the priority goes back up, so the
 * GOT_IP pick -- the one with an address to route from -- makes the
 * cable the default.
 *
 * "After" is got from esp_event's dispatch order rather than from
 * timing: handlers registered for one specific id run after every
 * handler registered for the whole base, whatever order they were
 * registered in. This one is for IOT_ETH_EVENT_CONNECTED alone; the
 * glue's is for all of IOT_ETH_EVENT.
 */
static void on_link_up_after(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;
    const iot_eth_handle_t h = data ? *(const iot_eth_handle_t *)data : NULL;
    for (int k = 0; k < IF_COUNT; k++) {
        if (h && s_if[k].eth == h && s_if[k].netif) {
            esp_netif_set_route_prio(s_if[k].netif, NETLINK_ETH_ROUTE_PRIO);
        }
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
static esp_err_t attach(eth_if_t *i, iot_eth_driver_t *driver, const char *if_key);

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
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IOT_ETH_EVENT, IOT_ETH_EVENT_CONNECTED,
                                                   on_link_up_after, NULL),
                        TAG, "link-up (after) handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                                   on_event, NULL),
                        TAG, "ETH_GOT_IP handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP,
                                                   on_event, NULL),
                        TAG, "ETH_LOST_IP handler");

    /* usbhost.c owns the stack. Installing it a second time here would
     * fail, and uninstalling it on a driver error would take the drive
     * and the headset with it. So both drivers are told to skip it.
     *
     * Each is tried on its own: an ASIX failure should not cost a
     * Realtek adapter its route, or the other way round. */
    const esp_usbh_asix_config_t asix_cfg = {
        .skip_init_usb_host_driver = true,
        .task_coreid = -1,
    };
    iot_eth_driver_t *driver = NULL;
    esp_err_t asix_err = esp_usbh_asix_new_eth(&asix_cfg, &driver);
    if (asix_err == ESP_OK) asix_err = attach(&s_if[IF_ASIX], driver, "USB_ASIX");
    if (asix_err != ESP_OK) {
        ESP_LOGE(TAG, "ASIX driver unavailable (%s)", esp_err_to_name(asix_err));
    }

    /* CDC-ECM: Realtek RTL8152/8153 in their second configuration (see
     * ethcfg.h) and anything else that is ECM as it comes. The match is
     * any device; iot_usbh_ecm then looks for an ECM interface and
     * ignores the drive and the headset, which have none. */
    const usbh_cdc_driver_config_t cdc_cfg = {
        .task_stack_size = 4096,
        .task_priority = 5,
        .task_coreid = -1,
        .skip_init_usb_host_driver = true,
    };
    esp_err_t ecm_err = usbh_cdc_driver_install(&cdc_cfg);
    if (ecm_err == ESP_OK) {
        const iot_usbh_ecm_config_t ecm_cfg = {
            .match_id_list = ESP_USB_DEVICE_MATCH_ID_ANY,
        };
        driver = NULL;
        ecm_err = iot_eth_new_usb_ecm(&ecm_cfg, &driver);
        if (ecm_err == ESP_OK) ecm_err = attach(&s_if[IF_ECM], driver, "USB_ECM");
    }
    if (ecm_err != ESP_OK) {
        ESP_LOGE(TAG, "CDC-ECM driver unavailable (%s)", esp_err_to_name(ecm_err));
    }

    if (asix_err != ESP_OK && ecm_err != ESP_OK) return asix_err;
    ESP_LOGI(TAG, "USB Ethernet ready (%s%s%s)",
             asix_err == ESP_OK ? "ASIX AX88772/A/B" : "",
             asix_err == ESP_OK && ecm_err == ESP_OK ? ", " : "",
             ecm_err == ESP_OK ? "CDC-ECM incl. RTL8152/8153" : "");
    return ESP_OK;
}

/* One driver onto its own iot_eth handle and netif, and started. */
static esp_err_t attach(eth_if_t *i, iot_eth_driver_t *driver, const char *if_key)
{
    const iot_eth_config_t eth_cfg = {
        .driver = driver,
        .stack_input = NULL,
    };
    iot_eth_handle_t eth = NULL;
    ESP_RETURN_ON_ERROR(iot_eth_install(&eth_cfg, &eth), TAG, "iot_eth_install");
    i->eth = eth;

    esp_netif_inherent_config_t inherent = ESP_NETIF_INHERENT_DEFAULT_ETH();
    inherent.if_key = if_key;
    inherent.if_desc = i->name;
    /* See netlink.h: the stock 50 loses to the station's 100, and a
     * cable nobody is using is not what plugging one in asks for. */
    inherent.route_prio = NETLINK_ETH_ROUTE_PRIO;
    const esp_netif_config_t netif_cfg = {
        .base = &inherent,
        .driver = NULL,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };
    i->netif = esp_netif_new(&netif_cfg);
    if (!i->netif) return ESP_ERR_NO_MEM;

    iot_eth_netif_glue_handle_t glue = iot_eth_new_netif_glue(eth);
    if (!glue) return ESP_ERR_NO_MEM;
    ESP_RETURN_ON_ERROR(esp_netif_attach(i->netif, glue), TAG, "netif attach");

    return iot_eth_start(eth);
}

esp_err_t ethernet_init(void)
{
    return usbhost_register_class("eth", eth_class_install);
}
