/*
 * audio_out.c -- I2S, the ES8388, and the speaker/headphone arbitration.
 *
 * Moved out of player.c unchanged apart from the names and one thing:
 * speaker_set() does a read-modify-write on the expander's output
 * register rather than rewriting PI4IOE1_OUT_SET wholesale. The old
 * version had to know the whole-byte value that io_expanders_init()
 * writes -- the panel and touch resets included -- to change one bit of
 * it, which is a second copy of a constant in a different file, and a
 * second copy is the thing that drifts. Reading the register back and
 * clearing bit 1 needs to know only about bit 1.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio_out.h"

static const char *TAG = "tab5_audio";

#define I2C_TIMEOUT_MS          (1000)

/* ---- I2S ---- */
#define I2S_MCLK_GPIO           (GPIO_NUM_30)
#define I2S_BCLK_GPIO           (GPIO_NUM_27)
#define I2S_LRCK_GPIO           (GPIO_NUM_29)
#define I2S_DOUT_GPIO           (GPIO_NUM_26)   /* DSDIN: P4 -> codec */

/* ---- ES8388 ---- */
#define ES8388_ADDR             (0x10)

/* ---- PI4IOE5V6416 expander 1 ---- */
#define PI4IOE_REG_OUT_SET      (0x05)
#define PI4IOE_REG_INPUT        (0x0F)
#define SPK_EN_BIT              (1 << 1)        /* P1 */

/*
 * SPK_EN (expander 1 P1) gates the NS4150B, and nothing gates it
 * automatically: with headphones in, the amp keeps driving the speaker
 * in parallel.
 *
 * Detect is expander 1 P7, active high -- set when a plug is in.
 * PI4IOE1_IO_DIR is 0x7F, so bit 7 is the one pin on that expander
 * configured as an input; the other seven are the resets and enables.
 *
 * Set HP_DETECT_MASK to 0 to get the identification mode back (logs the
 * expander's input register twice a second).
 */
#define HP_DETECT_MASK          (0x80)
#define HP_DETECT_ACTIVE_LOW    (0)
#define HP_POLL_MS              (200)

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_exp1;
static i2c_master_dev_handle_t s_es8388;
static i2s_chan_handle_t       s_tx;

static volatile bool s_headphones;
static uint32_t s_rate;
static uint8_t  s_channels;
static uint8_t  s_volume = 50;

/* ------------------------------------------------------------------ */
/* I2C helpers                                                         */
/* ------------------------------------------------------------------ */

static esp_err_t reg_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

static esp_err_t reg_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(dev, &reg, 1, val, 1, I2C_TIMEOUT_MS);
}

/* ------------------------------------------------------------------ */
/* I2S                                                                 */
/* ------------------------------------------------------------------ */

static esp_err_t i2s_init(uint32_t rate)
{
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.dma_desc_num = 8;
    chan.dma_frame_num = 480;
    chan.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan, &s_tx, NULL), TAG, "i2s_new_channel");

    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK_GPIO,
            .bclk = I2S_BCLK_GPIO,
            .ws   = I2S_LRCK_GPIO,
            .dout = I2S_DOUT_GPIO,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { false, false, false },
        },
    };
    /* ES8388 wants MCLK = 256 * Fs. */
    std.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std), TAG, "init_std");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "i2s_enable");
    return ESP_OK;
}

static esp_err_t i2s_set_rate(uint32_t rate)
{
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    clk.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ESP_RETURN_ON_ERROR(i2s_channel_disable(s_tx), TAG, "disable");
    ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_clock(s_tx, &clk), TAG, "reconfig");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "enable");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* ES8388 -- DAC playback path only                                    */
/* ------------------------------------------------------------------ */

/* vol: 0 = 0 dB, 33 = -99 dB (ES8388 LDACVOL/RDACVOL are 0.5 dB steps,
 * 0x00 loudest). Output mixer volume (reg 46/47) is a separate 0..0x21. */
static esp_err_t es8388_set_volume(uint8_t percent)
{
    if (percent > 100) percent = 100;
    uint8_t v = (uint8_t)((100 - percent) * 0x21 / 100);   /* 0 loud .. 0x21 mute */
    ESP_RETURN_ON_ERROR(reg_write(s_es8388, 46, 0x21 - v), TAG, "LOUT1VOL");
    ESP_RETURN_ON_ERROR(reg_write(s_es8388, 47, 0x21 - v), TAG, "ROUT1VOL");
    ESP_RETURN_ON_ERROR(reg_write(s_es8388, 48, 0x21 - v), TAG, "LOUT2VOL");
    ESP_RETURN_ON_ERROR(reg_write(s_es8388, 49, 0x21 - v), TAG, "ROUT2VOL");
    return ESP_OK;
}

static esp_err_t es8388_init(void)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES8388_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &cfg, &s_es8388), TAG,
                        "es8388 add");

    /* Reset, then out of reset. */
    ESP_RETURN_ON_ERROR(reg_write(s_es8388, 0, 0x80), TAG, "reset");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(reg_write(s_es8388, 0, 0x00), TAG, "unreset");

    /* Codec in slave mode, DAC only. */
    reg_write(s_es8388, 8,  0x00);  /* MASTERMODE: slave */
    reg_write(s_es8388, 2,  0xF3);  /* power down DEM/STM while configuring */
    reg_write(s_es8388, 1,  0x50);  /* ChipPower: analog on, ibias normal */
    reg_write(s_es8388, 3,  0xFC);  /* ADC fully powered down */
    reg_write(s_es8388, 0,  0x06);  /* internal VREF, DACMCLK from MCLK pin */

    /* DAC: 16-bit I2S, no de-emphasis. */
    reg_write(s_es8388, 23, 0x18);  /* DACCONTROL1: I2S, 16 bit */
    reg_write(s_es8388, 24, 0x02);  /* DACCONTROL2: DACFsRatio 256 */
    reg_write(s_es8388, 26, 0x00);  /* LDACVOL 0 dB */
    reg_write(s_es8388, 27, 0x00);  /* RDACVOL 0 dB */
    reg_write(s_es8388, 43, 0x80);  /* DACCONTROL21: DAC/ADC same LRCK, mixer on */

    /* Output mixer: DAC straight to LOUT/ROUT, no ADC path. */
    reg_write(s_es8388, 38, 0x09);
    reg_write(s_es8388, 39, 0x90);  /* LD2LO on, LI2LO off */
    reg_write(s_es8388, 42, 0x90);  /* RD2RO on, RI2RO off */

    /* Power up DAC L/R and both output pairs:
     *   OUT1 = headphone jack, OUT2 = NS4150B speaker amp. */
    reg_write(s_es8388, 4,  0x3C);
    reg_write(s_es8388, 2,  0x00);  /* release DEM/STM */

    ESP_RETURN_ON_ERROR(es8388_set_volume(s_volume), TAG, "volume");
    ESP_LOGI(TAG, "ES8388 initialised (0x%02X)", ES8388_ADDR);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Speaker / headphone                                                 */
/* ------------------------------------------------------------------ */

/*
 * Read-modify-write on one bit rather than a rewrite of the whole
 * register.
 *
 * The old version wrote PI4IOE1_OUT_SET (0x76) or that value with bit 1
 * cleared, which meant this file had to know the byte that carries
 * LCD_RST, TP_RST, CAM_RST and EXT5V as well -- four things it has no
 * business asserting an opinion about -- in order to change the one bit
 * it does own. Any future change to that constant would have had to be
 * made in two places or the panel would come out of reset differently
 * depending on whether headphones were in.
 */
static esp_err_t speaker_set(bool on)
{
    uint8_t val = 0;
    ESP_RETURN_ON_ERROR(reg_read(s_exp1, PI4IOE_REG_OUT_SET, &val), TAG, "spk read");
    val = on ? (val | SPK_EN_BIT) : (val & (uint8_t)~SPK_EN_BIT);
    return reg_write(s_exp1, PI4IOE_REG_OUT_SET, val);
}

static void headphone_task(void *arg)
{
    (void)arg;
    int last = -1;
    while (1) {
        uint8_t in1 = 0;
        reg_read(s_exp1, PI4IOE_REG_INPUT, &in1);

        if (HP_DETECT_MASK == 0) {
            /* Identification mode: watch this while plugging in. */
            ESP_LOGI(TAG, "expander input: 0x43=0x%02X", in1);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        const bool raw = (in1 & HP_DETECT_MASK) != 0;
        const int plugged = HP_DETECT_ACTIVE_LOW ? !raw : raw;
        if (plugged != last) {
            last = plugged;
            s_headphones = plugged;
            speaker_set(!plugged);
            ESP_LOGI(TAG, "headphones %s, speaker %s",
                     plugged ? "in" : "out", plugged ? "muted" : "on");
        }
        vTaskDelay(pdMS_TO_TICKS(HP_POLL_MS));
    }
}

const char *audio_out_route_name(void)
{
    return s_headphones ? "headphones" : "speaker";
}

/* ------------------------------------------------------------------ */

esp_err_t audio_out_set_format(uint32_t rate, uint8_t channels)
{
    if (rate == 0 || channels == 0) return ESP_ERR_INVALID_ARG;
    if (rate == s_rate && channels == s_channels) return ESP_OK;

    ESP_RETURN_ON_ERROR(i2s_set_rate(rate), TAG, "rate");
    s_rate = rate;
    s_channels = channels;
    return ESP_OK;
}

esp_err_t audio_out_write(const void *data, size_t len)
{
    size_t written = 0;
    return i2s_channel_write(s_tx, data, len, &written, portMAX_DELAY);
}

esp_err_t audio_out_set_volume(uint8_t percent)
{
    if (percent > 100) percent = 100;
    s_volume = percent;
    return es8388_set_volume(percent);
}

esp_err_t audio_out_init(i2c_master_bus_handle_t bus,
                         i2c_master_dev_handle_t exp1,
                         uint32_t rate)
{
    s_bus = bus;
    s_exp1 = exp1;

    /* MCLK must be running before the codec's DAC comes up: the ES8388
     * will not answer sensibly on I2C without it. */
    ESP_RETURN_ON_ERROR(i2s_init(rate), TAG, "i2s");
    s_rate = rate;
    s_channels = 2;

    ESP_RETURN_ON_ERROR(es8388_init(), TAG, "es8388");

    if (xTaskCreate(headphone_task, "hp_det", 3072, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
