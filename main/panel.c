/*
 * panel.c -- the settings panel.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"

#include "audio_out.h"
#include "gfx.h"
#include "panel.h"
#include "settings.h"
#include "storage.h"
#include "uac.h"
#include "usbhost.h"

static const char *TAG = "tab5_panel";

/* Lifted from browser.c rather than shared through a header, and that is
 * a decision rather than an oversight: two screens that happen to look
 * alike today are not one screen, and the first time the panel wants a
 * different row colour, a shared palette becomes a change to the
 * chooser. Six #defines is a cheaper coupling than a header. */
#define C_BG        RGB(0x0C, 0x0C, 0x0C)
#define C_ROW       RGB(0x1A, 0x1A, 0x1A)
#define C_ROW_ALT   RGB(0x14, 0x14, 0x14)
#define C_TEXT      RGB(0xEE, 0xEE, 0xEE)
#define C_DIM       RGB(0x77, 0x77, 0x77)
#define C_DISABLED  RGB(0x44, 0x44, 0x44)
#define C_ACCENT    RGB(0xD1, 0x3B, 0x2C)
#define C_ON        RGB(0x3C, 0xB3, 0x71)
#define C_TAB_ON    RGB(0x22, 0x22, 0x22)
#define C_TAB_OFF   RGB(0x10, 0x10, 0x10)
#define C_BTN       RGB(0x26, 0x26, 0x26)
#define C_RULE      RGB(0x33, 0x33, 0x33)

#define TAB_H       (96)
#define LIST_TOP    (TAB_H + 24)
#define FOOT_H      (120)
#define ROW_H       (64)
#define LABEL_SCALE (2)
#define NAME_SCALE  (3)

/* Long enough for a 36-character USB product string at scale 2, which is
 * the widest thing any of these rows carries. */
#define VAL_MAX     (48)

/*
 * How often the panel redraws itself with nothing having been touched.
 *
 * The heap figures move on their own, the USB port can enumerate a drive
 * while the panel is up, and the audio route changes when a headset is
 * plugged in. One second is slow enough to be free -- the whole draw is
 * a few hundred rectangles into a frame buffer -- and fast enough that
 * nothing on screen is visibly behind the hardware.
 */
#define REFRESH_MS  (1000)

typedef enum {
    TAB_SD = 0,
    TAB_USB,
    TAB_BUILD,
    TAB_AUDIO,
    TAB_COUNT
} panel_tab_t;

static const char *const k_tab_name[TAB_COUNT] = { "SD", "USB", "BUILD", "AUDIO" };

static bool        s_open;
static bool        s_dirty;
static bool        s_was_down;
static panel_tab_t s_tab = TAB_SD;
static uint32_t    s_seen_generation = UINT32_MAX;
static TickType_t  s_last_draw;

bool panel_is_open(void) { return s_open; }

void panel_open(void)
{
    s_open = true;
    s_dirty = true;
    s_was_down = false;
    /* Not reset to TAB_SD. Coming back to the tab you were last on is
     * the difference between a panel you check twice and one you
     * navigate twice. */
}

void panel_close(void)
{
    s_open = false;
}

/* ------------------------------------------------------------------ */
/* Rows                                                                */
/* ------------------------------------------------------------------ */

/*
 * A tab is a list of label/value pairs, built fresh on every draw.
 *
 * Built rather than cached because every value on these tabs is live --
 * the mount state, the heap, the route -- and a cache would need
 * invalidating from four other files. The whole build is a dozen
 * snprintf calls once a second.
 */
typedef struct {
    char label[24];
    char value[VAL_MAX];
    /* Drawn dimmer: a value that is absent rather than a value that is
     * zero. "no card" is not a reading of 0 MB. */
    bool absent;
} row_t;

#define ROWS_MAX (12)

static int row_add(row_t *rows, int n, const char *label, bool absent,
                   const char *fmt, ...) __attribute__((format(printf, 5, 6)));

static int row_add(row_t *rows, int n, const char *label, bool absent,
                   const char *fmt, ...)
{
    if (n >= ROWS_MAX) return n;
    snprintf(rows[n].label, sizeof(rows[n].label), "%s", label);

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(rows[n].value, sizeof(rows[n].value), fmt, ap);
    va_end(ap);

    rows[n].absent = absent;
    return n + 1;
}

static int build_sd(row_t *rows)
{
    storage_sd_info_t sd;
    storage_sd_info(&sd);

    int n = 0;
    n = row_add(rows, n, "state", !sd.present, "%s",
                sd.present ? "mounted" : "no card in the slot");
    if (!sd.present) return n;

    n = row_add(rows, n, "mount", false, "%s", storage_mount_path(STORAGE_SD));
    n = row_add(rows, n, "name", false, "%s", sd.name[0] ? sd.name : "-");
    n = row_add(rows, n, "type", false, "%s", sd.type);
    n = row_add(rows, n, "capacity", false, "%llu MB",
                (unsigned long long)sd.capacity_mb);
    /* The negotiated clock and the width together, because they multiply:
     * 4-bit at 40 MHz and 1-bit at 40 MHz are a factor of four apart and
     * only one of them is the bus working. */
    n = row_add(rows, n, "speed", false, "%d kHz, %d-bit",
                sd.speed_khz, sd.bus_width);
    return n;
}

static int build_usb(row_t *rows)
{
    storage_usb_info_t usb;
    storage_usb_info(&usb);

    int n = 0;
    /* Row 0 is the switch. The text still says what the port is doing --
     * the pill drawn over it says what it has been asked to do, and
     * "asked for, still coming up" is a real state worth being able to
     * see the difference of. */
    n = row_add(rows, n, "port", !usbhost_vbus_on(), "%s",
                !usbhost_vbus_on() ? "off"
                : usbhost_running() ? "on"
                                    : "coming up");

    /*
     * The audio device is on this tab and not on AUDIO, because the
     * question it answers is "did the thing I plugged in enumerate" --
     * which is a port question. AUDIO is where preferences live.
     */
    n = row_add(rows, n, "headset", !uac_present(), "%s",
                uac_present() ? (uac_product() ? uac_product() : "connected")
                              : "none");
    n = row_add(rows, n, "route", false, "%s", audio_out_route_name());

    n = row_add(rows, n, "drive", !usb.present, "%s",
                usb.present ? "mounted"
                            : usb.powered ? "waiting for a drive" : "-");
    if (!usb.present) return n;

    n = row_add(rows, n, "mount", false, "%s", storage_mount_path(STORAGE_USB));
    n = row_add(rows, n, "product", false, "%s",
                usb.product[0] ? usb.product : "-");
    n = row_add(rows, n, "maker", false, "%s",
                usb.manufacturer[0] ? usb.manufacturer : "-");
    n = row_add(rows, n, "id", false, "%04X:%04X", usb.vid, usb.pid);
    n = row_add(rows, n, "capacity", false, "%llu MB",
                (unsigned long long)usb.capacity_mb);
    n = row_add(rows, n, "sector", false, "%u bytes",
                (unsigned)usb.sector_size);
    return n;
}

static int build_build(row_t *rows)
{
    const esp_app_desc_t *d = esp_app_get_description();

    int n = 0;
    n = row_add(rows, n, "version", false, "%s", d ? d->version : "?");
    n = row_add(rows, n, "built", false, "%s %s",
                d ? d->date : "?", d ? d->time : "");
    n = row_add(rows, n, "idf", false, "%s", d ? d->idf_ver : IDF_VER);

    /*
     * Free heap in both places, because they fail differently and the
     * distinction is the first thing worth knowing. Internal RAM is what
     * runs out when too many tasks have stacks; PSRAM is what the rings
     * and the cover decoder live in, and 7 MB of rings against a total
     * that has drifted down is how a leak in the art path would show.
     */
    n = row_add(rows, n, "heap", false, "%u KB free",
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    n = row_add(rows, n, "psram", false, "%u KB free",
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    n = row_add(rows, n, "uptime", false, "%u s",
                (unsigned)(pdTICKS_TO_MS(xTaskGetTickCount()) / 1000));
    return n;
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

static void draw_tab(panel_tab_t id, int x, int w)
{
    const bool active = (id == s_tab);
    gfx_fill_rect(x, 0, w, TAB_H, active ? C_TAB_ON : C_TAB_OFF);
    if (active) gfx_fill_rect(x, TAB_H - 5, w, 5, C_ACCENT);

    const char *label = k_tab_name[id];
    const int tw = gfx_text_w(label, LABEL_SCALE);
    gfx_draw_text(x + (w - tw) / 2, (TAB_H - GFX_GLYPH_H(LABEL_SCALE)) / 2,
                  label, LABEL_SCALE, w - 8, active ? C_TEXT : C_DIM);
}

static void draw_rows(const row_t *rows, int n)
{
    const int w = gfx_w();
    for (int i = 0; i < n; i++) {
        const int y = LIST_TOP + i * ROW_H;
        gfx_fill_rect(0, y, w, ROW_H, (i & 1) ? C_ROW_ALT : C_ROW);

        const int ty = y + (ROW_H - GFX_GLYPH_H(LABEL_SCALE)) / 2;
        gfx_draw_text(24, ty, rows[i].label, LABEL_SCALE, 220, C_DIM);

        /* Right-aligned, and the tail kept when it will not fit: the end
         * of a product string or a path is the part that identifies it,
         * which is the same argument the chooser's path line makes. */
        const int avail = w - 260;
        int tw = gfx_text_w(rows[i].value, LABEL_SCALE);
        if (tw > avail) tw = avail;
        gfx_draw_text_tail(w - 24 - tw, ty, rows[i].value, LABEL_SCALE, avail,
                           rows[i].absent ? C_DISABLED : C_TEXT);
    }
}

/*
 * The AUDIO tab, which is the only one with a control on it.
 *
 * One switch, drawn as a full-width row with the state as a coloured
 * pill on the right. Not a checkbox: a checkbox needs a tick glyph the
 * 8x8 font does not have, and a word that says ON or OFF is unambiguous
 * at arm's length in a way a small square is not.
 */
/*
 * The USB power switch occupies row 0 of the USB tab.
 *
 * On a row rather than in the footer because it belongs to what it
 * switches: the reading directly under it is the drive that will go away
 * when it is turned off, which is the sentence the layout should be
 * making.
 */
static void usb_switch_box(int *x, int *y, int *w, int *h)
{
    *x = 0;
    *y = LIST_TOP;
    *w = gfx_w();
    *h = ROW_H;
}

static void draw_usb_switch(void)
{
    int x, y, bw, bh;
    usb_switch_box(&x, &y, &bw, &bh);

    const bool on = usbhost_vbus_on();
    /* Greyed while a track is playing off the drive. The refusal exists
     * either way -- storage_usb_power() enforces it -- but a button that
     * looks live and then does nothing is indistinguishable from a
     * broken one. */
    const bool locked = storage_usb_busy();

    const char *state = locked ? "IN USE" : on ? "ON" : "OFF";
    const int pw = 132, ph = 44;
    const int px = gfx_w() - 24 - pw, py = y + (bh - ph) / 2;

    gfx_fill_rect(px, py, pw, ph, locked ? C_TAB_OFF : on ? C_ON : C_BTN);
    const int tw = gfx_text_w(state, LABEL_SCALE);
    gfx_draw_text(px + (pw - tw) / 2, py + (ph - GFX_GLYPH_H(LABEL_SCALE)) / 2,
                  state, LABEL_SCALE, pw - 8,
                  locked ? C_DISABLED : on ? C_BG : C_DIM);
}

static void rg_switch_box(int *x, int *y, int *w, int *h)
{
    *x = 0;
    *y = LIST_TOP;
    *w = gfx_w();
    *h = ROW_H + 24;
}

static void draw_audio(void)
{
    const int w = gfx_w();
    const bool on = settings_rg_enabled();

    int x, y, bw, bh;
    rg_switch_box(&x, &y, &bw, &bh);

    gfx_fill_rect(x, y, bw, bh, C_ROW);
    gfx_draw_text(24, y + (bh - GFX_GLYPH_H(NAME_SCALE)) / 2, "ReplayGain",
                  NAME_SCALE, 400, C_TEXT);

    const char *state = on ? "ON" : "OFF";
    const int pw = 132, ph = 56;
    const int px = w - 24 - pw, py = y + (bh - ph) / 2;
    gfx_fill_rect(px, py, pw, ph, on ? C_ON : C_BTN);
    const int tw = gfx_text_w(state, NAME_SCALE);
    gfx_draw_text(px + (pw - tw) / 2, py + (ph - GFX_GLYPH_H(NAME_SCALE)) / 2,
                  state, NAME_SCALE, pw - 8, on ? C_BG : C_DIM);

    /*
     * What the switch actually does, said on the screen rather than left
     * to be discovered. "ReplayGain: OFF" with no explanation invites
     * the reasonable and wrong guess that existing measurements have
     * been thrown away.
     */
    static const char *const note[] = {
        "Levels every track to the same loudness.",
        "Off stops both measuring and applying;",
        "measurements already on the card are kept.",
        "Takes effect at the next track.",
    };
    int ny = y + bh + 32;
    for (unsigned i = 0; i < sizeof(note) / sizeof(note[0]); i++) {
        gfx_draw_text(24, ny, note[i], LABEL_SCALE, w - 48, C_DIM);
        ny += GFX_GLYPH_H(LABEL_SCALE) + 12;
    }
}

void panel_draw(void)
{
    if (!s_open) return;

    const uint32_t gen = storage_generation();
    if (gen != s_seen_generation) {
        s_seen_generation = gen;
        s_dirty = true;
    }

    const TickType_t now = xTaskGetTickCount();
    if ((now - s_last_draw) >= pdMS_TO_TICKS(REFRESH_MS)) s_dirty = true;

    if (!s_dirty) return;
    s_dirty = false;
    s_last_draw = now;

    const int w = gfx_w(), h = gfx_h();
    gfx_fill_rect(0, 0, w, h, C_BG);

    const int tw = w / TAB_COUNT;
    for (int i = 0; i < TAB_COUNT; i++) {
        draw_tab((panel_tab_t)i, i * tw,
                 (i == TAB_COUNT - 1) ? (w - i * tw) : tw);
    }
    gfx_fill_rect(0, LIST_TOP - 2, w, 2, C_RULE);

    if (s_tab == TAB_AUDIO) {
        draw_audio();
    } else {
        row_t rows[ROWS_MAX];
        memset(rows, 0, sizeof(rows));
        const int n = (s_tab == TAB_SD)  ? build_sd(rows)
                    : (s_tab == TAB_USB) ? build_usb(rows)
                                         : build_build(rows);
        draw_rows(rows, n);
        if (s_tab == TAB_USB) draw_usb_switch();
    }

    const int fy = h - FOOT_H;
    gfx_fill_rect(0, fy, w, FOOT_H, C_BG);
    gfx_fill_rect(0, fy, w, 2, C_RULE);

    /* One button, and it is the way out. Everything else on this screen
     * is either a tab or the switch. */
    gfx_fill_rect(w / 2 - 90, fy + 16, 180, FOOT_H - 32, C_BTN);
    const int cw = gfx_text_w("CLOSE", LABEL_SCALE);
    gfx_draw_text(w / 2 - cw / 2,
                  fy + 16 + (FOOT_H - 32 - GFX_GLYPH_H(LABEL_SCALE)) / 2,
                  "CLOSE", LABEL_SCALE, 172, C_TEXT);

    gfx_blit(0, h);
}

/* ------------------------------------------------------------------ */
/* Touch                                                               */
/* ------------------------------------------------------------------ */

bool panel_touch(bool down, int x, int y)
{
    const bool tapped = down && !s_was_down;
    s_was_down = down;

    if (!s_open || !tapped) return false;

    const int w = gfx_w(), h = gfx_h();

    if (y < TAB_H) {
        int which = x / (w / TAB_COUNT);
        if (which < 0) which = 0;
        if (which >= TAB_COUNT) which = TAB_COUNT - 1;
        if ((panel_tab_t)which != s_tab) {
            s_tab = (panel_tab_t)which;
            s_dirty = true;
        }
        ESP_LOGI(TAG, "button: tab %s", k_tab_name[s_tab]);
        return false;
    }

    if (y >= h - FOOT_H) {
        ESP_LOGI(TAG, "button: close");
        return true;
    }

    if (s_tab == TAB_USB) {
        int bx, by, bw, bh;
        usb_switch_box(&bx, &by, &bw, &bh);
        if (y >= by && y < by + bh) {
            const bool want = !usbhost_vbus_on();
            if (storage_usb_power(want)) {
                ESP_LOGI(TAG, "USB bus power %s", want ? "on" : "off");
            }
            /* Redrawn either way, including on a refusal: the state has
             * not changed but the press must produce something, and the
             * IN USE pill is the answer to why. */
            s_dirty = true;
        }
        return false;
    }

    if (s_tab == TAB_AUDIO) {
        int bx, by, bw, bh;
        rg_switch_box(&bx, &by, &bw, &bh);
        /* The whole row, not just the pill. A 132 px target beside 400 px
         * of dead label is the kind of thing that reads as the button
         * being broken when the press lands 20 px to the left. */
        if (y >= by && y < by + bh) {
            const bool on = !settings_rg_enabled();
            settings_set_rg_enabled(on);
            ESP_LOGI(TAG, "replaygain %s (next track)", on ? "on" : "off");
            s_dirty = true;
        }
    }
    return false;
}
