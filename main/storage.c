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

#include "usb/msc_host.h"
#include "usb/msc_host_vfs.h"

#include "storage.h"
#include "usbhost.h"

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

/* Files a volume must hold open at once -- see MAX_OPEN_FILES below.
 *
 * Bus power and the host stack are NOT here any more. USB5V_EN, the
 * usb_host_install() call and the library task moved to usbhost.c when a
 * second class driver appeared on the same port; this file installs the
 * mass-storage class driver and owns the mounts, and nothing else.
 */

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

static volatile bool s_mounted[STORAGE_COUNT];
static volatile uint32_t s_generation;
static volatile int s_held = STORAGE_COUNT;

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

bool storage_is_hidden(const char *name)
{
    return !name || name[0] == '.';
}

uint32_t storage_generation(void) { return s_generation; }

void storage_hold(storage_id_t id) { s_held = id; }

/* Answered by the bus owner. Kept as a one-line forward rather than
 * deleted so browser.c does not have to learn about usbhost.c to ask a
 * question about the USB volume. */
bool storage_usb_powered(void) { return usbhost_powered(); }

/* ------------------------------------------------------------------ */
/* microSD                                                             */
/* ------------------------------------------------------------------ */

static esp_err_t sd_mount(bool verbose)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;          /* the default is slot 1 */
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
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

/* Runs on the class driver's own task; does nothing but forward. */
static void msc_event_cb(const msc_host_event_t *event, void *arg)
{
    (void)arg;
    if (s_msc_events) xQueueSend(s_msc_events, event, 0);
}

/* Registered with usbhost.c and called by it, on the bus task, after the
 * host stack is installed and before VBUS goes high. */
static esp_err_t msc_class_install(void)
{
    const msc_host_driver_config_t msc_cfg = {
        .create_backround_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .callback = msc_event_cb,
    };
    return msc_host_install(&msc_cfg);
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

esp_err_t storage_init(void)
{
    s_msc_events = xQueueCreate(4, sizeof(msc_host_event_t));
    if (!s_msc_events) return ESP_ERR_NO_MEM;
    ESP_RETURN_ON_ERROR(usbhost_register_class("msc", msc_class_install),
                        TAG, "register msc");

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
     * The port is not conditional any more.
     *
     * It used to come up only when there was no card at boot or when the
     * USB tab was tapped -- both of which are questions about where the
     * FILES are. A USB audio device is not a file source, and it cannot
     * announce itself through a dark port: with a card in the slot and
     * nobody in the chooser, a headset plugged into this player would
     * have been invisible for as long as the card kept working.
     *
     * So app_main() powers the port at boot and this file no longer has
     * an opinion about it. What is lost is a milliamp or two on a board
     * with nothing plugged in, which is the state the port was in
     * anyway; what is gained is that the highest-priority output can be
     * detected at all.
     */

    if (xTaskCreate(storage_task, "storage", 4096, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
