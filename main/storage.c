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
#include "ff.h"
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

/*
 * The FAT hidden bit, for the files this program leaves lying around.
 *
 * A leading dot means "hidden" to us and to every Unix, and nothing at
 * all to Windows or to the file browser on a phone. The card ends up
 * with .defeatist.dat and a .rgcache next to every track, all of them
 * plainly visible on the machine most likely to be looking at them, and
 * a stranger's answer to a directory full of dotfiles is usually to
 * delete them.
 *
 * FatFs can set the attribute; the VFS layer has no call for it, so
 * this reaches past the VFS to f_chmod(). That needs the path as FatFs
 * sees it -- a drive number and the path with the mount point removed
 * -- and IDF does not publish which drive a mount was given. Rather
 * than assume registration order (which puts the drive somewhere
 * different depending on whether a card was present at boot), try each
 * one and take the first that finds the file. FF_VOLUMES is 2 here, so
 * this is at most one wasted call.
 *
 * Advisory throughout. A card that will not take the attribute, a
 * FatFs built without f_chmod, a path on neither volume: the file is
 * still written and still works, it is just visible.
 */
void storage_mark_hidden(const char *path)
{
#if FF_USE_CHMOD
    const storage_id_t id = storage_of_path(path);
    if (id == STORAGE_COUNT) return;

    const char *mount = storage_mount_path(id);
    if (!mount) return;

    const char *rel = path + strlen(mount);     /* keeps the leading '/' */
    if (*rel != '/') return;

    for (int drv = 0; drv < FF_VOLUMES; drv++) {
        char ff_path[640];
        if (snprintf(ff_path, sizeof(ff_path), "%d:%s", drv, rel)
            >= (int)sizeof(ff_path)) {
            return;
        }
        if (f_chmod(ff_path, AM_HID, AM_HID) == FR_OK) return;
    }

    ESP_LOGD(TAG, "could not hide %s", path);
#else
    /*
     * Logged once rather than per file: a build without f_chmod writes
     * one of these per sidecar otherwise, which is noise about a
     * cosmetic failure.
     */
    static bool said;
    if (!said) {
        said = true;
        ESP_LOGI(TAG, "FatFs built without f_chmod; dotfiles stay visible on FAT");
    }
    (void)path;
#endif
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

/*
 * Try High Speed, fall back to the default.
 *
 * The mount was pinned at SDMMC_FREQ_DEFAULT, which is 20 MHz, and at
 * 4-bit that is a 10 MB/s ceiling on a card the log reports as SDHC --
 * a class of card that supports the 25 MB/s High Speed mode. Half the
 * bus was being left alone for no stated reason.
 *
 * A fallback rather than a straight bump, because whether 40 MHz works
 * is a property of the board's card slot and its routing, not of the
 * card, and neither is visible from here. A player that will not mount
 * is worse than a slow one -- which is the whole argument of this
 * project -- so a failure at 40 retries at 20 before giving up, and the
 * log says which one answered.
 *
 * `speed` in the mount banner is `s_card->max_freq_khz`, the negotiated
 * rate rather than the requested one, so a card that declines High Speed
 * reports what it actually settled on.
 */
static esp_err_t sd_mount_at(bool verbose, int freq_khz);

/*
 * Latched, not re-tried per mount. sd_mount() is the 1 Hz poll, and an
 * empty slot fails it by timing out -- so a blind "try fast, then try
 * slow" would double the cost of the commonest case in the program,
 * which is nobody having put a card in. It would also re-probe 40 MHz
 * once a second forever on a board that cannot do it.
 */
static int s_sd_freq_khz = SDMMC_FREQ_HIGHSPEED;

static esp_err_t sd_mount(bool verbose)
{
    const esp_err_t err = sd_mount_at(verbose, s_sd_freq_khz);
    if (err == ESP_OK) return ESP_OK;

    /*
     * Only a card that answered and then failed is evidence about the
     * clock. ESP_ERR_TIMEOUT and ESP_ERR_NOT_FOUND are an empty slot,
     * which is the poll's normal state and says nothing; ESP_FAIL is
     * "no mountable filesystem", which is a formatting problem and
     * would produce the same complaint at half the speed.
     */
    if (err == ESP_ERR_TIMEOUT || err == ESP_ERR_NOT_FOUND || err == ESP_FAIL) {
        return err;
    }

    if (s_sd_freq_khz == SDMMC_FREQ_DEFAULT) return err;

    ESP_LOGW(TAG, "mount failed at %d kHz (%s); dropping to %d kHz for good",
             s_sd_freq_khz, esp_err_to_name(err), SDMMC_FREQ_DEFAULT);
    s_sd_freq_khz = SDMMC_FREQ_DEFAULT;
    return sd_mount_at(verbose, s_sd_freq_khz);
}

static esp_err_t sd_mount_at(bool verbose, int freq_khz)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;          /* the default is slot 1 */
    host.max_freq_khz = freq_khz;
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

bool storage_usb_busy(void)
{
    return s_held == STORAGE_USB && s_mounted[STORAGE_USB];
}

bool storage_usb_power(bool on)
{
    if (on) {
        usbhost_set_power(true);
        return true;
    }

    /*
     * Refused rather than forced. A held volume means the decode loop is
     * inside a read on it, and the only ways to make this safe are to
     * stop the track or to wait for it -- one of which is a decision
     * that belongs to the listener and the other of which would block
     * the UI task on a card read.
     */
    if (storage_usb_busy()) {
        ESP_LOGW(TAG, "USB power off refused: a track is playing from %s",
                 STORAGE_USB_MOUNT);
        return false;
    }

    /*
     * Unmount before the power goes, not after.
     *
     * After is what a physical unplug does, and it works only because
     * the MSC driver reports the disconnect and storage_task tears the
     * mount down in response. Doing it deliberately, we can do it in the
     * right order instead: VFS unregistered, device uninstalled, and
     * only then the line dropped -- so there is no window in which a
     * mounted filesystem is sitting on a dead bus.
     *
     * From the caller's task rather than deferred to storage_task,
     * because the caller is a button press and the alternative is a flag
     * that the poll task services up to a second later, with the panel
     * showing the old state for all of it. usb_detach() touches only
     * this file's own handles and the flags, and the poll task's other
     * work is the card.
     */
    if (s_mounted[STORAGE_USB] || s_msc_dev) usb_detach();

    usbhost_set_power(false);
    return true;
}

/* ------------------------------------------------------------------ */
/* Snapshots, for the settings panel                                   */
/* ------------------------------------------------------------------ */

/*
 * Read from the caller's task, off structures the poll task owns.
 *
 * There is no lock, and the reason it is safe is the same reason
 * storage_present() has none: the pointers are set before s_mounted goes
 * true and cleared after it goes false, both on one task, and a torn
 * read here costs one frame of a stale capacity figure on a panel that
 * redraws every second. A mutex would put the drawing task behind a
 * mount, which can take hundreds of milliseconds on a slow card.
 *
 * The pointer is sampled once into a local. Testing s_card and then
 * dereferencing s_card is two reads of something another task can
 * NULL in between; testing a local cannot be.
 */
void storage_sd_info(storage_sd_info_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));

    sdmmc_card_t *const c = s_card;
    if (!c || !s_mounted[STORAGE_SD]) return;

    out->present = true;
    snprintf(out->name, sizeof(out->name), "%s", c->cid.name);
    snprintf(out->type, sizeof(out->type), "%s",
             c->is_mmc ? "MMC/eMMC"
                       : (c->ocr & SD_OCR_CCS_BIT) ? "SDHC/SDXC" : "SDSC");
    out->capacity_mb = ((uint64_t)c->csd.capacity * c->csd.sector_size)
                       / (1024 * 1024);
    /* The negotiated clock, not the card's printed rating -- which is
     * the number worth showing, because a card that came up at 20 MHz on
     * a bus that can do 40 is the answer to a stuttering track. */
    out->speed_khz = c->max_freq_khz;
    out->bus_width = c->log_bus_width ? 4 : 1;
}

/* The descriptor strings are UTF-16 in the descriptor and wchar_t here.
 * Taken as the low byte of each unit, which is exact for the ASCII every
 * drive actually uses and produces a readable approximation of anything
 * else -- against a panel field that is 36 bytes and a font that has no
 * glyphs beyond Latin-1 either way. */
static void wide_to_ascii(char *dst, size_t dst_len, const wchar_t *src)
{
    size_t i = 0;
    if (!dst_len) return;
    if (src) {
        for (; i + 1 < dst_len && src[i]; i++) {
            const unsigned c = (unsigned)src[i];
            dst[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
        }
    }
    dst[i] = '\0';
}

void storage_usb_info(storage_usb_info_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));

    out->powered = storage_usb_powered();

    msc_host_device_handle_t const dev = s_msc_dev;
    if (!dev || !s_mounted[STORAGE_USB]) return;

    msc_host_device_info_t info;
    if (msc_host_get_device_info(dev, &info) != ESP_OK) return;

    out->present = true;
    out->vid = info.idVendor;
    out->pid = info.idProduct;
    out->sector_size = info.sector_size;
    out->capacity_mb = ((uint64_t)info.sector_count * info.sector_size)
                       / (1024 * 1024);
    wide_to_ascii(out->product, sizeof(out->product), info.iProduct);
    wide_to_ascii(out->manufacturer, sizeof(out->manufacturer), info.iManufacturer);
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
