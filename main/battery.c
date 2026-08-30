/*
 * battery.c -- see battery.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "battery.h"

static const char *TAG = "tab5_batt";

/* INA226 register map, and the two LSB sizes from the datasheet. */
#define REG_CONFIG      (0x00)
#define REG_SHUNT       (0x01)
#define REG_BUS         (0x02)
#define REG_ID          (0xFF)

#define BUS_LSB_UV      (1250)      /* 1.25 mV */
#define SHUNT_LSB_NV    (2500)      /* 2.5 uV */

/*
 * Averaging 16 samples, 1.1 ms conversions, shunt and bus, continuous.
 *
 * Averaging in the part rather than in software: the ripple this is
 * trying not to show is the speaker amplifier's, at audio rates, and the
 * place to reject that is the sampler.
 */
#define CONFIG_VALUE    (0x4527)

/*
 * The trace's configuration: no averaging, 140 us bus conversions, bus
 * only. See battery_trace_arm() in the header for why the normal one is
 * useless here.
 *
 *   [11:9] AVG     000  1 sample
 *   [8:6]  VBUSCT  000  140 us
 *   [5:3]  VSHCT   000  140 us (unused; mode is bus only)
 *   [2:0]  MODE    110  bus voltage, continuous
 *
 * Shunt is dropped so the part is not spending half its time on a
 * channel this does not read, which halves the interval between bus
 * samples.
 */
#define TRACE_CONFIG_VALUE  (0x4006)

/* Samples per trace, and how long to keep taking them. 600 ms covers a
 * seek commit with room either side; the buffer is sized so a faster
 * bus cannot overrun it. 1200 x 2 bytes is 2.4 KB of .bss. */
#define TRACE_SAMPLES       (1200)
#define TRACE_WINDOW_MS     (600)

static volatile bool  s_trace_wanted;
static const char    *volatile s_trace_why = "";
static uint16_t       s_trace_raw[TRACE_SAMPLES];
static TaskHandle_t   s_trace_task;

/*
 * Which sign of shunt voltage means current INTO the pack.
 *
 * Negative, on this board. R39 sits between the pack and the system with
 * IN+ on the pack side, so ordinary discharge reads positive and a
 * charger pushing current backwards through it reads negative. M5Stack's
 * own configuration for this board says the same thing in as many words.
 *
 * It is a named constant rather than a bare comparison because it is a
 * board fact, not a law: this is the one line to change if the fill
 * turns green exactly when it should not.
 */
#define BATTERY_CHARGE_SIGN     (-1)

/*
 * TWO lithium cells in series, resting, as eight points.
 *
 * An NP-F550 is 2S: about 8.2 V full and about 6.0 V at the point the
 * board gives up. These are single-cell figures doubled, with the bottom
 * anchored at 6.0 V rather than at 2 x 3.0 V, because the shutdown
 * threshold is what "empty" means to someone holding the thing -- a
 * gauge that reads 8% as the player dies is wrong in the only way that
 * matters.
 *
 * Not a formula, because there is not one -- the middle of the curve is
 * nearly flat and the ends fall off a cliff, which is exactly what a
 * table is good at and a polynomial is bad at. Linear between points.
 */
typedef struct { int mv; int pct; } point_t;

static const point_t k_curve[] = {
    { 6000,   0 }, { 7100,  10 }, { 7320,  20 }, { 7480,  40 },
    { 7700,  60 }, { 7900,  75 }, { 8140,  90 }, { 8300, 100 },
};

static int curve_pct(int mv)
{
    const int n = sizeof(k_curve) / sizeof(k_curve[0]);
    if (mv <= k_curve[0].mv) return 0;
    if (mv >= k_curve[n - 1].mv) return 100;

    for (int i = 1; i < n; i++) {
        if (mv > k_curve[i].mv) continue;
        const point_t a = k_curve[i - 1], b = k_curve[i];
        return a.pct + ((mv - a.mv) * (b.pct - a.pct)) / (b.mv - a.mv);
    }
    return 100;
}

static i2c_master_dev_handle_t s_dev;

/* Published values. Ints, written by one task, read by any -- see the
 * threading note in the header. */
static volatile int  s_pct = -1;
static volatile int  s_mv;
static volatile bool s_charging;

/* The smoothed voltage, in millivolts, held here rather than recomputed:
 * an exponential average needs its own history and this is it. */
static int s_avg_mv;

static esp_err_t reg16_write(uint8_t reg, uint16_t val)
{
    const uint8_t buf[3] = { reg, (uint8_t)(val >> 8), (uint8_t)val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 1000);
}

static esp_err_t reg16_read(uint8_t reg, uint16_t *out)
{
    uint8_t rx[2];
    const esp_err_t err =
        i2c_master_transmit_receive(s_dev, &reg, 1, rx, sizeof(rx), 1000);
    if (err != ESP_OK) return err;
    *out = (uint16_t)((rx[0] << 8) | rx[1]);
    return ESP_OK;
}

esp_err_t battery_init(i2c_master_bus_handle_t bus)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BATTERY_INA226_ADDR,
        .scl_speed_hz = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (err != ESP_OK) return err;

    /* Ask before configuring. A device that does not answer here is an
     * absent gauge, which is a normal way for a bench board to be, and
     * is worth one line rather than an abort. */
    uint16_t id = 0;
    err = reg16_read(REG_ID, &id);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no gauge at 0x%02X (%s); no battery indicator",
                 BATTERY_INA226_ADDR, esp_err_to_name(err));
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "INA226 at 0x%02X (die id 0x%04X)",
             BATTERY_INA226_ADDR, id);
    return reg16_write(REG_CONFIG, CONFIG_VALUE);
}

static void battery_task(void *arg)
{
    (void)arg;

    while (1) {
        uint16_t bus = 0, shunt = 0;

        if (reg16_read(REG_BUS, &bus) == ESP_OK &&
            reg16_read(REG_SHUNT, &shunt) == ESP_OK) {

            const int mv = ((int)bus * BUS_LSB_UV) / 1000;

            /* Shunt voltage is signed two's complement; the sign is the
             * direction of the current and therefore the whole of the
             * charge detection. */
            const int16_t sv = (int16_t)shunt;
            const long shunt_uv = ((long)sv * SHUNT_LSB_NV) / 1000;

            /* A threshold rather than a bare sign test. At rest the
             * reading dithers around zero, and a bolt that flickers on a
             * player sitting on a desk is worse than no bolt. */
            /* 200 uV across 5 mohm is 40 mA -- comfortably above the
             * dither and well below any real charge current. */
            s_charging = (BATTERY_CHARGE_SIGN * shunt_uv) > 200;

            /* Exponential average, 1/8. Fast enough to follow a cable
             * being plugged in within a couple of seconds, slow enough
             * that the amplifier's draw does not show. */
            s_avg_mv = s_avg_mv ? s_avg_mv + (mv - s_avg_mv) / 8 : mv;
            s_mv = s_avg_mv;

            /* Rounded to 5. See the header: the precision is not there,
             * and displaying it anyway is what makes a gauge look
             * broken when it is merely approximate. */
            const int raw = curve_pct(s_avg_mv);
            s_pct = ((raw + 2) / 5) * 5;
            if (s_pct > 100) s_pct = 100;
        }

        /* 5 s. Nothing here changes faster than that, and each poll is
         * two I2C transactions on the bus the touch panel is sharing. */
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

/*
 * One trace: reconfigure, sample flat out, restore, then report.
 *
 * The report happens after the restore, deliberately. Logging inside the
 * window would put a USB-serial-JTAG write in the middle of the thing
 * being measured, and that write is itself a load.
 */
static void battery_trace_run(void)
{
    const char *why = s_trace_why;

    if (reg16_write(REG_CONFIG, TRACE_CONFIG_VALUE) != ESP_OK) {
        ESP_LOGW(TAG, "trace: could not reconfigure the gauge");
        return;
    }

    const TickType_t t0 = xTaskGetTickCount();
    int n = 0;
    while (n < TRACE_SAMPLES &&
           pdTICKS_TO_MS(xTaskGetTickCount() - t0) < TRACE_WINDOW_MS) {
        uint16_t bus = 0;
        if (reg16_read(REG_BUS, &bus) != ESP_OK) break;
        s_trace_raw[n++] = bus;
    }
    const uint32_t took = (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount() - t0);

    /* Back to the gauge's configuration before anything slow happens. */
    reg16_write(REG_CONFIG, CONFIG_VALUE);

    if (n <= 0) {
        ESP_LOGW(TAG, "trace (%s): no samples", why);
        return;
    }

    int lo = INT32_MAX, hi = 0;
    long sum = 0;
    int lo_at = 0;
    for (int i = 0; i < n; i++) {
        const int mv = ((int)s_trace_raw[i] * BUS_LSB_UV) / 1000;
        if (mv < lo) { lo = mv; lo_at = i; }
        if (mv > hi) hi = mv;
        sum += mv;
    }

    /*
     * The summary first, because it is the answer: a sag of tens of
     * millivolts is ordinary, and a sag of hundreds next to a flash is
     * the thing this was written to find. lo_at says where in the window
     * it happened, which is what lines it up against the seek.
     */
    ESP_LOGW(TAG, "trace (%s): %d samples in %" PRIu32 " ms, "
                  "min %d mV at %d ms, max %d mV, mean %d mV, sag %d mV",
             why, n, took, lo, (int)((long)lo_at * took / n), hi,
             (int)(sum / n), hi - lo);

    /*
     * Then the series, sixteen to a line, so the shape can be read
     * rather than inferred from three numbers. At warning level like the
     * summary: a trace that is armed and then filtered out by a log
     * level is the worst of both.
     */
    char line[16 * 6 + 1];
    for (int i = 0; i < n; i += 16) {
        int m = 0;
        line[0] = '\0';
        for (int j = i; j < n && j < i + 16; j++) {
            const int mv = ((int)s_trace_raw[j] * BUS_LSB_UV) / 1000;
            m += snprintf(line + m, sizeof(line) - m, "%d ", mv);
        }
        ESP_LOGW(TAG, "trace: %s", line);
    }
}

static void battery_trace_task(void *arg)
{
    (void)arg;
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        battery_trace_run();
        s_trace_wanted = false;
    }
}

void battery_trace_arm(const char *why)
{
    if (!s_dev || !s_trace_task) return;

    /* Ignored rather than queued. Two traces back to back would have the
     * second one measuring the first one's logging. */
    if (s_trace_wanted) return;

    s_trace_why = why ? why : "";
    s_trace_wanted = true;
    xTaskNotifyGive(s_trace_task);
}

esp_err_t battery_start(void)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;

    /* Above the gauge task and above the UI, so a trace is not
     * descheduled in the middle of the window it is measuring. Below the
     * writer, which must never wait for a diagnostic. */
    if (xTaskCreate(battery_trace_task, "batt_tr", 4096, NULL, 5,
                    &s_trace_task) != pdPASS) {
        ESP_LOGW(TAG, "no trace task; battery_trace_arm() will do nothing");
        s_trace_task = NULL;
    }

    return xTaskCreate(battery_task, "batt", 3072, NULL, 2, NULL) == pdPASS
           ? ESP_OK : ESP_ERR_NO_MEM;
}

int battery_pct(void)      { return s_pct; }
bool battery_charging(void){ return s_charging; }
int battery_mv(void)       { return s_mv; }
