/*
 * storage.c -- microSD and USB mass storage, mounted together.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "driver/sdmmc_host.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"

#include "usb/usb_host.h"
#include "usb/msc_host.h"
#include "usb/msc_host_vfs.h"

#include "storage.h"

static const char *TAG = "tab5_storage";

/* ---- microSD (SDMMC slot 0, 4-bit) ---- */
#define SD_CLK_GPIO             (GPIO_NUM_43)
#define SD_CMD_GPIO             (GPIO_NUM_44)
#define SD_D0_GPIO              (GPIO_NUM_39)
#define SD_D1_GPIO              (GPIO_NUM_40)
#define SD_D2_GPIO              (GPIO_NUM_41)
#define SD_D3_GPIO              (GPIO_NUM_42)
#define SD_LDO_CHAN             (4)

/* OCR bit 30, Card Capacity Status. IDF spells it SD_OCR_SDHC_CAP in
 * sd_protocol_defs.h, which sdmmc_cmd.h stopped pulling in on v6 and
 * which has already moved once. The bit is fixed by the SD physical
 * layer spec, so name it here. */
#define SD_OCR_CCS_BIT          (1UL << 30)

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

/* How often the card slot is looked at. There is no card-detect line on
 * this board -- M5's BSP passes GPIO_NUM_NC for it -- so presence is
 * polled or it is not known at all. One second is fast enough to feel
 * like hotplug and slow enough that the probe is not competing with the
 * decoder for the bus. */
#define POLL_MS                 (1000)

/* Files a volume must hold open at once. The decoder holds one, the
 * album-art reader briefly holds a second, and the chooser's scan holds a
 * DIR. Five is the IDF default and is enough; it is stated rather than
 * inherited so a later playlist prefetch does not quietly hit the
 * "no free file descriptors" wall. */
#define MAX_OPEN_FILES          (5)

/* ------------------------------------------------------------------ */

static sdmmc_card_t *s_card;
static sd_pwr_ctrl_handle_t s_pwr;
static i2c_master_dev_handle_t s_exp2;

static volatile bool s_mounted[STORAGE_COUNT];
static volatile uint32_t s_generation;
static volatile int s_held = STORAGE_COUNT;

/* The port is off until something asks. s_usb_want is the request, set
 * from the boot path or from a touch; s_usb_up is what the poll task has
 * actually done about it. Two flags rather than one because the work is
 * I2C plus a 100 ms settle and must not happen on the asking task. */
static volatile bool s_usb_want;
static bool s_usb_up;

static msc_host_device_handle_t s_msc_dev;
static msc_host_vfs_handle_t s_msc_vfs;
static QueueHandle_t s_msc_events;

bool storage_present(storage_id_t id)
{
    if (id < 0 || id >= STORAGE_COUNT) return false;
    return s_mounted[id];
}

const char *storage_mount_path(storage_id_t id)
{
    return (id == STORAGE_USB) ? STORAGE_USB_MOUNT : STORAGE_SD_MOUNT;
}

const char *storage_label(storage_id_t id)
{
    return (id == STORAGE_USB) ? "USB" : "microSD";
}

storage_id_t storage_of_path(const char *path)
{
    if (!path) return STORAGE_COUNT;
    if (strncmp(path, STORAGE_SD_MOUNT "/", sizeof(STORAGE_SD_MOUNT)) == 0 ||
        strcmp(path, STORAGE_SD_MOUNT) == 0) {
        return STORAGE_SD;
    }
    if (strncmp(path, STORAGE_USB_MOUNT "/", sizeof(STORAGE_USB_MOUNT)) == 0 ||
        strcmp(path, STORAGE_USB_MOUNT) == 0) {
        return STORAGE_USB;
    }
    return STORAGE_COUNT;
}

bool storage_join_path(char *out, size_t out_len, const char *dir, const char *name)
{
    if (!out || out_len == 0) return false;
    out[0] = '\0';
    if (!dir || !name) return false;

    const size_t dn = strlen(dir);
    const size_t nn = strlen(name);
    const size_t sep = (dn && dir[dn - 1] == '/') ? 0 : 1;

    if (dn + sep + nn + 1 > out_len) return false;

    memcpy(out, dir, dn);
    if (sep) out[dn] = '/';
    memcpy(out + dn + sep, name, nn + 1);
    return true;
}

uint32_t storage_generation(void) { return s_generation; }

void storage_hold(storage_id_t id) { s_held = id; }

void storage_usb_enable(void) { s_usb_want = true; }

/* True once the port has been *asked* for, not only once it is up.
 *
 * The poll task can be a second behind the tap, and for that second the
 * chooser would otherwise tell the user to tap again -- which either does
 * nothing or, worse, reads as the first tap having missed. */
bool storage_usb_powered(void) { return s_usb_up || s_usb_want; }

/* ------------------------------------------------------------------ */
/* microSD                                                             */
/* ------------------------------------------------------------------ */

static esp_err_t sd_mount(bool verbose)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;          /* the default is slot 1 */
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    host.pwr_ctrl_handle = s_pwr;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = SD_CLK_GPIO;
    slot.cmd = SD_CMD_GPIO;
    slot.d0  = SD_D0_GPIO;
    slot.d1  = SD_D1_GPIO;
    slot.d2  = SD_D2_GPIO;
    slot.d3  = SD_D3_GPIO;
    /* No card-detect or write-protect line on this board. */

    const esp_vfs_fat_sdmmc_mount_config_t mnt = {
        .format_if_mount_failed = false,
        .max_files = MAX_OPEN_FILES,
        .allocation_unit_size = 16 * 1024,
    };

    const esp_err_t ret = esp_vfs_fat_sdmmc_mount(STORAGE_SD_MOUNT, &host, &slot,
                                                  &mnt, &s_card);
    if (ret != ESP_OK) {
        /* A failed mount leaves the host initialised with its slot GPIOs
         * checked out; the next attempt then reports
         * "conflict found for GPIO[42]". Tear it down.
         *
         * This matters far more now than it did when the mount was tried
         * once at boot: the poll retries it every second forever, so a
         * leak here is a guaranteed failure a second later rather than a
         * one-off. */
        (void)sdmmc_host_deinit();
        s_card = NULL;
        if (verbose) {
            if (ret == ESP_FAIL) {
                ESP_LOGE(TAG, "card present but no mountable filesystem");
#ifndef CONFIG_FATFS_USE_EXFAT_VENDORED
                ESP_LOGE(TAG, "if this card is exFAT, run ./tools/enable_exfat.sh");
#endif
            } else {
                ESP_LOGD(TAG, "no card (%s)", esp_err_to_name(ret));
            }
        }
        return ret;
    }

    const uint64_t bytes = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
    ESP_LOGI(TAG, "microSD mounted at %s", STORAGE_SD_MOUNT);
    ESP_LOGI(TAG, "  %-12s %s", "name", s_card->cid.name);
    ESP_LOGI(TAG, "  %-12s %s", "type",
             s_card->is_mmc ? "MMC/eMMC"
                            : (s_card->ocr & SD_OCR_CCS_BIT) ? "SDHC/SDXC" : "SDSC");
    ESP_LOGI(TAG, "  %-12s %llu MB", "capacity", bytes / (1024 * 1024));
    ESP_LOGI(TAG, "  %-12s %d kHz", "speed", s_card->max_freq_khz);
    ESP_LOGI(TAG, "  %-12s %d-bit", "bus width", s_card->log_bus_width ? 4 : 1);
    return ESP_OK;
}

static void sd_unmount(void)
{
    if (!s_card) return;
    esp_vfs_fat_sdcard_unmount(STORAGE_SD_MOUNT, s_card);
    s_card = NULL;
    ESP_LOGI(TAG, "microSD removed");
}

/* ------------------------------------------------------------------ */
/* USB mass storage                                                    */
/* ------------------------------------------------------------------ */

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
    if (on) vTaskDelay(pdMS_TO_TICKS(USB_VBUS_SETTLE_MS));
    return ESP_OK;
}

/* Runs on the class driver's own task; does nothing but forward. */
static void msc_event_cb(const msc_host_event_t *event, void *arg)
{
    (void)arg;
    if (s_msc_events) xQueueSend(s_msc_events, event, 0);
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

/* Host stack, class driver, then bus power -- in that order, so the
 * enumeration of a drive that is already in the port is handled by a
 * stack that exists rather than dropped on the floor. */
static esp_err_t usb_bring_up(void)
{
    s_msc_events = xQueueCreate(4, sizeof(msc_host_event_t));
    if (!s_msc_events) return ESP_ERR_NO_MEM;

    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_RETURN_ON_ERROR(usb_host_install(&host_cfg), TAG, "usb_host_install");

    if (xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    const msc_host_driver_config_t msc_cfg = {
        .create_backround_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .callback = msc_event_cb,
    };
    ESP_RETURN_ON_ERROR(msc_host_install(&msc_cfg), TAG, "msc_host_install");

    return usb_vbus(true);
}

static void usb_attach(uint8_t addr)
{
    if (s_msc_dev) return;                  /* one drive at a time */

    esp_err_t err = msc_host_install_device(addr, &s_msc_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MSC device install failed (%s)", esp_err_to_name(err));
        s_msc_dev = NULL;
        return;
    }

    const esp_vfs_fat_mount_config_t mnt = {
        .format_if_mount_failed = false,
        .max_files = MAX_OPEN_FILES,
        .allocation_unit_size = 16 * 1024,
    };
    err = msc_host_vfs_register(s_msc_dev, STORAGE_USB_MOUNT, &mnt, &s_msc_vfs);
    if (err != ESP_OK) {
        /* Same failure mode as the card: a drive with no filesystem the
         * build can read. exFAT is the usual reason, and the usual fix is
         * the same script. */
        ESP_LOGE(TAG, "USB drive has no mountable filesystem (%s)",
                 esp_err_to_name(err));
#ifndef CONFIG_FATFS_USE_EXFAT_VENDORED
        ESP_LOGE(TAG, "if this drive is exFAT, run ./tools/enable_exfat.sh");
#endif
        msc_host_uninstall_device(s_msc_dev);
        s_msc_dev = NULL;
        s_msc_vfs = NULL;
        return;
    }

    s_mounted[STORAGE_USB] = true;
    s_generation++;
    ESP_LOGI(TAG, "USB drive mounted at %s", STORAGE_USB_MOUNT);
}

static void usb_detach(void)
{
    if (!s_msc_dev) return;
    if (s_msc_vfs) {
        msc_host_vfs_unregister(s_msc_vfs);
        s_msc_vfs = NULL;
    }
    msc_host_uninstall_device(s_msc_dev);
    s_msc_dev = NULL;
    s_mounted[STORAGE_USB] = false;
    s_generation++;
    ESP_LOGI(TAG, "USB drive removed");
}

/* ------------------------------------------------------------------ */

/*
 * One task owns both mounts.
 *
 * The card has to be polled -- there is no detect line -- and the drive
 * does not, but running the drive's teardown from here rather than from
 * the class driver's callback keeps every mount and unmount on a single
 * task. Otherwise the callback could unmount /usb while this task is
 * mid-mount on /sd, and both end up inside the same VFS registration
 * table.
 */
static void storage_task(void *arg)
{
    (void)arg;

    while (1) {
        if (s_usb_want && !s_usb_up) {
            /* The request is not cleared on the way in. Clearing it first
             * leaves a window where neither flag is set and the chooser
             * reverts its label to "tap to power" mid-bring-up; it is
             * cleared only on failure, which is exactly when that label
             * is the truth again. */
            const esp_err_t err = usb_bring_up();
            if (err == ESP_OK) {
                s_usb_up = true;
                /* The tab redraws to say "powered, waiting" rather than
                 * "tap to power", which is the whole point of having
                 * asked. */
                s_generation++;
            } else {
                s_usb_want = false;
                ESP_LOGW(TAG, "USB host unavailable (%s); microSD only",
                         esp_err_to_name(err));
            }
        }

        msc_host_event_t ev;
        while (s_msc_events && xQueueReceive(s_msc_events, &ev, 0) == pdTRUE) {
            if (ev.event == MSC_DEVICE_CONNECTED) {
                usb_attach(ev.device.address);
            } else if (ev.event == MSC_DEVICE_DISCONNECTED) {
                /* The drive is already gone; this only releases the
                 * bookkeeping. A held volume is still torn down, because
                 * unlike the card there is nothing left to read from and
                 * the handle is invalid either way. */
                usb_detach();
            }
        }

        if (s_mounted[STORAGE_SD]) {
            /* sdmmc_get_status() is a CMD13 at the card. It is the only
             * removal signal available, and it is why this loop is a
             * second rather than faster: an empty slot answers by timing
             * out. */
            if (sdmmc_get_status(s_card) != ESP_OK) {
                s_mounted[STORAGE_SD] = false;
                s_generation++;
                if (s_held == STORAGE_SD) {
                    /* Marked absent, not unmounted. The player is inside
                     * a read on this volume; it will see the flag, stop
                     * the track and release, and the next pass does the
                     * unmount for real. */
                    ESP_LOGW(TAG, "microSD pulled while playing");
                } else {
                    sd_unmount();
                }
            }
        } else if (s_card) {
            /* Absent but still mounted: the deferred unmount above. */
            if (s_held != STORAGE_SD) sd_unmount();
        } else if (sd_mount(false) == ESP_OK) {
            s_mounted[STORAGE_SD] = true;
            s_generation++;
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

esp_err_t storage_init(i2c_master_dev_handle_t exp2)
{
    s_exp2 = exp2;

    const sd_pwr_ctrl_ldo_config_t ldo = { .ldo_chan_id = SD_LDO_CHAN };
    ESP_RETURN_ON_ERROR(sd_pwr_ctrl_new_on_chip_ldo(&ldo, &s_pwr), TAG, "sd ldo");
    ESP_LOGI(TAG, "SDMMC IO power up (LDO ch%d)", SD_LDO_CHAN);

    /* First attempt is loud, so a card that is in the slot but unreadable
     * says so once instead of failing silently once a second forever. */
    if (sd_mount(true) == ESP_OK) {
        s_mounted[STORAGE_SD] = true;
        s_generation++;
    }

    /*
     * Rule 1: no card at boot, so look at the other port.
     *
     * Boot only, deliberately. A card pulled later does not light the
     * port -- that is a removal, not a search for media, and the chooser
     * is one tap away for anyone who disagrees. Tying it to removal would
     * also mean a card reseated twice had powered the port permanently as
     * a side effect.
     *
     * "Card present but unreadable" counts as no card here. The volume
     * did not mount, so there is nothing to play from it either way.
     */
    if (!s_mounted[STORAGE_SD]) {
        ESP_LOGI(TAG, "no card at boot; powering the USB-A port");
        storage_usb_enable();
    } else {
        ESP_LOGI(TAG, "card mounted; USB-A port left unpowered");
    }

    if (xTaskCreate(storage_task, "storage", 4096, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
