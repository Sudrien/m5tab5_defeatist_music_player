/*
 * usbhost.c -- the USB-A port: host stack, class drivers, bus power.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "usb/usb_host.h"

#include "usbhost.h"

static const char *TAG = "tab5_usbhost";

/* ---- USB-A bus power ----
 *
 * USB5V_EN is P3 of the PI4IOE5V6416 at 0x44 -- the second expander, not
 * the one carrying SPK_EN and the resets. Three registers, in this order,
 * and all three matter:
 *
 *   IO_DIR   (0x03) bit 3 = 1   make P3 an output
 *   OUT_H_IM (0x07) bit 3 = 0   take it out of high impedance
 *   OUT_SET  (0x05) bit 3 = 1   drive it high
 *
 * The high-impedance register is the one that is easy to miss: the
 * expander parks pins high-Z after reset, so writing OUT_SET alone leaves
 * P3 floating and VBUS off, with nothing on the P4 side reporting a
 * fault. This is the same trap io_expanders_init() already documents for
 * expander 1 and SPK_EN.
 *
 * player.c's PI4IOE2_IO_DIR (0xB9) already has bit 3 set, which is
 * exactly what distinguishes it from M5Unified's 0xB1 -- that value puts
 * P3 back to an input and the port stays dark. The direction write is
 * repeated here anyway so this file does not depend on that constant
 * keeping its current value.
 */
#define PI4IOE_REG_IO_DIR       (0x03)
#define PI4IOE_REG_OUT_SET      (0x05)
#define PI4IOE_REG_OUT_HIGH_Z   (0x07)
#define USB5V_EN_BIT            (1 << 3)
#define I2C_TIMEOUT_MS          (1000)

/* Long enough for the port's inrush to settle before the host stack
 * starts driving bus resets at whatever is plugged in. */
#define USB_VBUS_SETTLE_MS      (100)

/* Mass storage and audio. A third would be a HID remote and there is no
 * fourth in sight; this is a bound, not a budget. */
#define MAX_CLASSES             (4)

static i2c_master_dev_handle_t s_exp2;

static struct {
    const char      *name;
    usbhost_class_fn fn;
} s_class[MAX_CLASSES];
static size_t s_classes;

/* s_want is the request, set from anywhere; s_up is what the bus task
 * has actually done about it. Two flags rather than one because the work
 * is I2C plus a settle and must not happen on the asking task. */
static volatile bool s_want;
static volatile bool s_up;
static TaskHandle_t  s_task;

/*
 * VBUS, separately from the stack.
 *
 * s_vbus_want is the request and s_vbus_on is what the line is actually
 * doing, the same split as s_want/s_up above and for the same reason.
 * They are separate from that pair because the stack and the power are
 * now separate lifetimes: the stack goes up once and stays, and the
 * power can be cycled underneath it any number of times.
 */
static volatile bool s_vbus_want = true;
static volatile bool s_vbus_on;

bool usbhost_powered(void) { return s_up || s_want; }
bool usbhost_running(void) { return s_up; }
bool usbhost_vbus_on(void) { return s_vbus_on; }

esp_err_t usbhost_register_class(const char *name, usbhost_class_fn fn)
{
    if (!name || !fn) return ESP_ERR_INVALID_ARG;
    /* Late registration is refused rather than honoured. A class driver
     * installed after enumeration has already happened will not be
     * offered the devices that are already attached, so it would sit
     * there looking installed and see nothing until someone unplugged
     * and replugged -- which is a worse failure than being told no. */
    if (s_up || s_want) return ESP_ERR_INVALID_STATE;
    if (s_classes >= MAX_CLASSES) return ESP_ERR_NO_MEM;

    s_class[s_classes].name = name;
    s_class[s_classes].fn = fn;
    s_classes++;
    return ESP_OK;
}

static esp_err_t usb_vbus(bool on)
{
    if (!s_exp2) return ESP_ERR_INVALID_STATE;

    uint8_t reg, val;
    esp_err_t err;

    struct { uint8_t reg; bool set; } steps[] = {
        { PI4IOE_REG_IO_DIR,     true  },   /* output          */
        { PI4IOE_REG_OUT_HIGH_Z, false },   /* out of high-Z   */
        { PI4IOE_REG_OUT_SET,    on    },   /* drive           */
    };

    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        reg = steps[i].reg;
        err = i2c_master_transmit_receive(s_exp2, &reg, 1, &val, 1, I2C_TIMEOUT_MS);
        if (err != ESP_OK) return err;
        val = steps[i].set ? (val | USB5V_EN_BIT) : (val & (uint8_t)~USB5V_EN_BIT);
        const uint8_t buf[2] = { reg, val };
        err = i2c_master_transmit(s_exp2, buf, sizeof(buf), I2C_TIMEOUT_MS);
        if (err != ESP_OK) return err;
    }

    ESP_LOGI(TAG, "USB-A VBUS %s (expander 0x44, P3)", on ? "on" : "off");
    s_vbus_on = on;
    if (on) vTaskDelay(pdMS_TO_TICKS(USB_VBUS_SETTLE_MS));
    return ESP_OK;
}

static void usb_lib_task(void *arg)
{
    (void)arg;
    while (1) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

/* Host stack, class drivers, then bus power -- in that order, so a
 * device already in the port is enumerated by a stack that exists rather
 * than dropped on the floor. */
static esp_err_t bring_up(void)
{
    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_RETURN_ON_ERROR(usb_host_install(&host_cfg), TAG, "usb_host_install");

    if (xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    /* One class failing does not take the port down for the others: a
     * headset should still play on a build where the MSC driver could
     * not allocate, and a drive should still mount on one where the
     * audio driver could not. */
    for (size_t i = 0; i < s_classes; i++) {
        const esp_err_t err = s_class[i].fn();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "class driver %s installed", s_class[i].name);
        } else {
            ESP_LOGE(TAG, "class driver %s failed (%s)", s_class[i].name,
                     esp_err_to_name(err));
        }
    }

    return usb_vbus(true);
}

/*
 * The bus task exists only so that usbhost_start() can return
 * immediately. Its callers are the boot path and, potentially, a touch
 * event; neither should block for the inrush settle, and one of them is
 * the UI task.
 *
 * It parks on a notification rather than polling, and after a successful
 * bring-up it has nothing left to do -- the class drivers own their own
 * event tasks from there.
 */
static void usbhost_task(void *arg)
{
    (void)arg;
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        /*
         * Power, when the stack is already up. Checked first, because
         * once s_up is set the bring-up below never runs again and this
         * is the only thing left for this task to do.
         */
        if (s_up && s_vbus_want != s_vbus_on) {
            const esp_err_t err = usb_vbus(s_vbus_want);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "VBUS %s failed (%s)",
                         s_vbus_want ? "on" : "off", esp_err_to_name(err));
            }
            continue;
        }

        if (!s_want || s_up) continue;

        /* Asked for with the power off -- bring the stack up but leave
         * the line alone, so a port switched off before it was ever
         * started does not come up anyway. */
        if (!s_vbus_want) {
            ESP_LOGI(TAG, "USB host start deferred: bus power is off");
            continue;
        }

        const esp_err_t err = bring_up();
        if (err == ESP_OK) {
            s_up = true;
        } else {
            /* Cleared only on failure. Clearing it on the way in would
             * leave a window where neither flag is set and anything
             * drawing a label off usbhost_powered() reverts to "off"
             * mid-bring-up. */
            s_want = false;
            ESP_LOGW(TAG, "USB host unavailable (%s)", esp_err_to_name(err));
        }
    }
}

esp_err_t usbhost_init(i2c_master_dev_handle_t exp2)
{
    s_exp2 = exp2;
    if (xTaskCreate(usbhost_task, "usbhost", 4096, NULL, 3, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void usbhost_start(void)
{
    if (s_up) return;
    s_want = true;
    if (s_task) xTaskNotifyGive(s_task);
}

void usbhost_set_power(bool on)
{
    if (on == s_vbus_want) return;
    s_vbus_want = on;
    /* A port that was never started still needs starting when it is
     * switched on: the stack comes up, then the line. */
    if (on) s_want = true;
    if (s_task) xTaskNotifyGive(s_task);
}
