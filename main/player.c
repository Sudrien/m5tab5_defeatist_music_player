/*
 * M5Stack Tab5 (ESP32-P4) -- play the first playable track on the microSD
 * card.
 *
 * Plain ESP-IDF 5.5.x. No M5Unified, no BSP, no ESP-ADF pipeline. Decode
 * goes through decoder.c (minimp3 for MP3, esp_audio_codec for FLAC,
 * WAV, M4A, AAC and friends); output is I2S -> ES8388 -> 3.5mm jack /
 * NS4150B speaker.
 *
 * Board details that are easy to miss, in the same spirit as the display
 * and USB examples:
 *
 *   1. SPK_EN is not a GPIO. Like LCD_RST and TP_RST, it sits on the
 *      PI4IOE5V6416 I/O expander at I2C 0x43 -- pin P1. The display
 *      example's PI4IOE1_OUT_SET (0x76) already drives it high, so if you
 *      merge these projects you get it for free. Standalone, we set it
 *      here. Without it the codec plays into a disabled amplifier and you
 *      hear nothing, with no error anywhere.
 *
 *   2. SDMMC IO power is an on-chip LDO, channel 4. Skip
 *      sd_pwr_ctrl_new_on_chip_ldo() and the bus lines have no supply, so
 *      every card reads as absent -- identical to an empty slot.
 *
 *   3. The ES8388 needs MCLK running before it will answer sensibly on
 *      I2S. We start the I2S channel (which drives MCLK) before writing
 *      the codec's DAC power-up registers.
 *
 * SPDX-License-Identifier: MIT
 */

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/sdmmc_host.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"

#include "freertos/stream_buffer.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7121.h"
#include "esp_ldo_regulator.h"
#include "esp_idf_version.h"

#include "albumart.h"
#include "decoder.h"

static const char *TAG = "tab5_mp3";

/* ---- I2C bus: same pins the display example uses ---- */
#define BSP_I2C_NUM             (0)
#define BSP_I2C_SDA             (GPIO_NUM_31)
#define BSP_I2C_SCL             (GPIO_NUM_32)
#define I2C_TIMEOUT_MS          (1000)

/* ---- PI4IOE5V6416 expander 1: P1 = SPK_EN ---- */
#define PI4IOE_ADDR_1           (0x43)
#define PI4IOE_REG_IO_DIR       (0x03)
#define PI4IOE_REG_OUT_SET      (0x05)
#define PI4IOE_REG_OUT_HIGH_Z   (0x07)
#define PI4IOE1_IO_DIR          (0x7F)
#define PI4IOE1_OUT_SET         (0x76)  /* P1 SPK_EN, P2 EXT5V, P4 LCD_RST, P5 TP_RST, P6 CAM_RST */

/* ---- ES8388 ---- */
#define ES8388_ADDR             (0x10)

/* ---- Display: ST7121 MIPI-DSI, portrait native ---- */
#define LCD_H_RES               (720)
#define LCD_V_RES               (1280)
#define LCD_BITS_PER_PIXEL      (24)
#define DSI_DATA_LANES          (2)
#define DSI_LANE_RATE_MBPS      (965)
#define DPI_CLOCK_MHZ           (70)
#define DSI_PHY_LDO_CHAN        (3)
#define DSI_PHY_LDO_VOLTAGE_MV  (2500)

#define LCD_BACKLIGHT_GPIO      (GPIO_NUM_22)
#define LCD_LEDC_CHANNEL        (LEDC_CHANNEL_1)
#define LCD_LEDC_TIMER          (LEDC_TIMER_0)
#define LCD_LEDC_DUTY_RES       (LEDC_TIMER_12_BIT)
#define LCD_LEDC_DUTY_MAX       (4095)
#define LCD_LEDC_FREQ_HZ        (5000)
#define LCD_BRIGHTNESS_PERCENT  (80)

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
#define DPI_COLOR_FORMAT        (LCD_COLOR_FMT_RGB565)
#else
#define DPI_COLOR_FORMAT        (LCD_COLOR_PIXEL_FORMAT_RGB565)
#endif

#define PI4IOE_REG_CHIP_RESET   (0x01)
#define PI4IOE_REG_PULL_EN      (0x0B)
#define PI4IOE_REG_PULL_SEL     (0x0D)

/* Input status. The PI4IOE5V6408 map used here runs Chip Reset 0x01,
 * IO Dir 0x03, Output 0x05, High-Z 0x07, Pull Enable 0x0B, Pull Select
 * 0x0D, Input Status 0x0F -- so this is the next register along. If the
 * probe below reads a constant, this address is the first thing to
 * doubt. */
#define PI4IOE_REG_INPUT        (0x0F)
#define PI4IOE_ADDR_2           (0x44)
#define PI4IOE2_IO_DIR          (0xB9)

/* ---- Audio plumbing ----
 *
 * The decoder and the I2S writer are separate tasks with a stream
 * buffer between them. In one loop, every SD refill and every MP3 frame
 * happened between two i2s_channel_write() calls, so the DMA ran dry on
 * each one -- audible as a steady tick. Decoupled, the writer only ever
 * waits on DMA and the decoder can stall for a whole SD read without
 * anyone hearing it.
 *
 * 64 KB is ~0.37 s of 44.1 kHz stereo: comfortably longer than a FAT
 * cluster read, short enough that seeking will not feel laggy later. */
#define PCM_RING_BYTES          (64 * 1024)
#define PCM_CHUNK_BYTES         (4 * 1024)

/* ---- I2S ---- */
#define I2S_MCLK_GPIO           (GPIO_NUM_30)
#define I2S_BCLK_GPIO           (GPIO_NUM_27)
#define I2S_LRCK_GPIO           (GPIO_NUM_29)
#define I2S_DOUT_GPIO           (GPIO_NUM_26)   /* DSDIN: P4 -> codec */

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
#define SD_MOUNT                "/sd"

/* Input buffering now lives inside decoder.c -- minimp3_ex does its own
 * over its IO callbacks, and the esp_audio_codec path keeps a sliding
 * window. Only the PCM side is sized here. */

static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_exp1, s_exp2;
static i2c_master_dev_handle_t s_es8388;
static i2s_chan_handle_t s_tx;

/* ------------------------------------------------------------------ */
/* I2C helpers                                                         */
/* ------------------------------------------------------------------ */

static esp_err_t reg_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

static esp_err_t i2c_bus_init(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = BSP_I2C_NUM,
        .sda_io_num = BSP_I2C_SDA,
        .scl_io_num = BSP_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, &s_i2c_bus);
}

static esp_err_t add_dev(uint8_t addr, i2c_master_dev_handle_t *out)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    return i2c_master_bus_add_device(s_i2c_bus, &cfg, out);
}

/*
 * Expander 1 carries P1 SPK_EN, P2 EXT5V_EN, P4 LCD_RST, P5 TP_RST and
 * P6 CAM_RST. PI4IOE1_OUT_SET (0x76) drives all of them high in one
 * write, so the amplifier and the panel come out of reset together.
 *
 * Both OUT_SET (0x05) and OUT_H_IM (0x07) matter: the expander parks
 * pins high-impedance after reset, so writing the value alone leaves
 * them floating -- amp off, panel dead, no error from either.
 */
static esp_err_t io_expanders_init(void)
{

    ESP_RETURN_ON_ERROR(add_dev(PI4IOE_ADDR_1, &s_exp1), TAG, "expander 0x43 absent");
    ESP_RETURN_ON_ERROR(reg_write(s_exp1, PI4IOE_REG_CHIP_RESET, 0xFF), TAG, "reset 1");
    ESP_RETURN_ON_ERROR(reg_write(s_exp1, PI4IOE_REG_IO_DIR, PI4IOE1_IO_DIR), TAG, "dir 1");
    ESP_RETURN_ON_ERROR(reg_write(s_exp1, PI4IOE_REG_OUT_HIGH_Z, 0x00), TAG, "high-z 1");
    ESP_RETURN_ON_ERROR(reg_write(s_exp1, PI4IOE_REG_PULL_SEL, 0x7F), TAG, "pull sel 1");
    ESP_RETURN_ON_ERROR(reg_write(s_exp1, PI4IOE_REG_PULL_EN, 0x7F), TAG, "pull en 1");
    ESP_RETURN_ON_ERROR(reg_write(s_exp1, PI4IOE_REG_OUT_SET, PI4IOE1_OUT_SET), TAG, "out 1");
    ESP_LOGI(TAG, "SPK_EN and LCD_RST released (expander 0x%02X)", PI4IOE_ADDR_1);

    ESP_RETURN_ON_ERROR(add_dev(PI4IOE_ADDR_2, &s_exp2), TAG, "expander 0x44 absent");
    ESP_RETURN_ON_ERROR(reg_write(s_exp2, PI4IOE_REG_CHIP_RESET, 0xFF), TAG, "reset 2");
    ESP_RETURN_ON_ERROR(reg_write(s_exp2, PI4IOE_REG_IO_DIR, PI4IOE2_IO_DIR), TAG, "dir 2");

    vTaskDelay(pdMS_TO_TICKS(100));     /* panel out of reset before first command */
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Speaker / headphone                                                 */
/* ------------------------------------------------------------------ */

/*
 * SPK_EN (expander 1 P1) gates the NS4150B, and nothing gates it
 * automatically: with headphones in, the amp keeps driving the speaker
 * in parallel.
 *
 * Detect is expander 1 P7, active high -- set when a plug is
 * in. PI4IOE1_IO_DIR is 0x7F, so bit 7 is the one pin on that expander
 * configured as an input; the other seven are the resets and enables.
 *
 * Set HP_DETECT_MASK to 0 to get the identification mode back (logs
 * both expanders' input registers twice a second).
 */
#define HP_DETECT_MASK          (0x80)
#define HP_DETECT_ACTIVE_LOW    (0)

static esp_err_t reg_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(dev, &reg, 1, val, 1, I2C_TIMEOUT_MS);
}

static esp_err_t speaker_set(bool on)
{
    const uint8_t out = on ? PI4IOE1_OUT_SET : (PI4IOE1_OUT_SET & ~0x02);
    return reg_write(s_exp1, PI4IOE_REG_OUT_SET, out);
}

static void headphone_task(void *arg)
{
    int last = -1;
    while (1) {
        uint8_t in1 = 0, in2 = 0;
        reg_read(s_exp1, PI4IOE_REG_INPUT, &in1);
        reg_read(s_exp2, PI4IOE_REG_INPUT, &in2);

        if (HP_DETECT_MASK == 0) {
            /* Identification mode: watch these while plugging in. */
            ESP_LOGI(TAG, "expander inputs: 0x43=0x%02X 0x44=0x%02X", in1, in2);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        (void)in2;
        const bool raw = (in1 & HP_DETECT_MASK) != 0;
        const int plugged = HP_DETECT_ACTIVE_LOW ? !raw : raw;
        if (plugged != last) {
            last = plugged;
            speaker_set(!plugged);
            ESP_LOGI(TAG, "headphones %s, speaker %s",
                     plugged ? "in" : "out", plugged ? "muted" : "on");
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/* ------------------------------------------------------------------ */
/* Display                                                             */
/* ------------------------------------------------------------------ */

static esp_lcd_panel_handle_t s_panel;

static esp_err_t backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LCD_LEDC_DUTY_RES,
        .timer_num = LCD_LEDC_TIMER,
        .freq_hz = LCD_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "ledc timer");

    const ledc_channel_config_t ch = {
        .gpio_num = LCD_BACKLIGHT_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LCD_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LCD_LEDC_TIMER,
        .duty = 0,                      /* dark until the panel is up */
        .hpoint = 0,
    };
    return ledc_channel_config(&ch);
}

static esp_err_t backlight_set(int percent)
{
    percent = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
    const uint32_t duty = (LCD_LEDC_DUTY_MAX * percent) / 100;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_LEDC_CHANNEL, duty),
                        TAG, "duty");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_LEDC_CHANNEL);
}

static esp_err_t panel_init(void)
{
    esp_ldo_channel_handle_t phy_ldo = NULL;
    esp_lcd_dsi_bus_handle_t dsi = NULL;
    esp_lcd_panel_io_handle_t io = NULL;

    const esp_ldo_channel_config_t ldo = {
        .chan_id = DSI_PHY_LDO_CHAN,
        .voltage_mv = DSI_PHY_LDO_VOLTAGE_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo, &phy_ldo), TAG, "phy ldo");

    const esp_lcd_dsi_bus_config_t bus = {
        .bus_id = 0,
        .num_data_lanes = DSI_DATA_LANES,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = DSI_LANE_RATE_MBPS,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus, &dsi), TAG, "dsi bus");

    const esp_lcd_dbi_io_config_t dbi = {
        .virtual_channel = 0, .lcd_cmd_bits = 8, .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(dsi, &dbi, &io), TAG, "panel io");

    esp_lcd_dpi_panel_config_t dpi = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = DPI_CLOCK_MHZ,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
        .in_color_format = DPI_COLOR_FORMAT,
#else
        .pixel_format = DPI_COLOR_FORMAT,
#endif
        .num_fbs = 1,
        .video_timing = {
            .h_size = LCD_H_RES, .v_size = LCD_V_RES,
            .hsync_pulse_width = 2,  .hsync_back_porch = 40, .hsync_front_porch = 40,
            .vsync_pulse_width = 20, .vsync_back_porch = 24, .vsync_front_porch = 200,
        },
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0)
        .flags.use_dma2d = true,
#endif
    };

    st7121_vendor_config_t vendor = {
        .init_cmds = NULL, .init_cmds_size = 0,
        .mipi_config = { .dsi_bus = dsi, .dpi_config = &dpi },
    };
    const esp_lcd_panel_dev_config_t pcfg = {
        .reset_gpio_num = -1,           /* released via expander P4 */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = LCD_BITS_PER_PIXEL,
        .vendor_config = &vendor,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7121(io, &pcfg, &s_panel), TAG, "st7121");
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_enable_dma2d(s_panel), TAG, "dma2d");
#endif
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "disp on");
    ESP_LOGI(TAG, "ST7121 initialised (%dx%d)", LCD_H_RES, LCD_V_RES);
    return ESP_OK;
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
    ESP_RETURN_ON_ERROR(add_dev(ES8388_ADDR, &s_es8388), TAG, "es8388 add");

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

    ESP_RETURN_ON_ERROR(es8388_set_volume(70), TAG, "volume");
    ESP_LOGI(TAG, "ES8388 initialised (0x%02X)", ES8388_ADDR);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* microSD                                                             */
/* ------------------------------------------------------------------ */

static sdmmc_card_t *s_card;
static sd_pwr_ctrl_handle_t s_pwr;

static esp_err_t sd_mount(void)
{
    const sd_pwr_ctrl_ldo_config_t ldo = { .ldo_chan_id = SD_LDO_CHAN };
    ESP_RETURN_ON_ERROR(sd_pwr_ctrl_new_on_chip_ldo(&ldo, &s_pwr), TAG, "sd ldo");
    ESP_LOGI(TAG, "SDMMC IO power up (LDO ch%d)", SD_LDO_CHAN);

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
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    const esp_err_t ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT, &host, &slot, &mnt, &s_card);
    if (ret != ESP_OK) {
        /* A failed mount leaves the host initialised with its slot GPIOs
         * checked out; the next attempt then reports
         * "conflict found for GPIO[42]". Tear it down. */
        (void)sdmmc_host_deinit();
        s_card = NULL;
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "card present but no mountable filesystem");
#ifndef CONFIG_FATFS_USE_EXFAT_VENDORED
            ESP_LOGE(TAG, "if this card is exFAT, run ./tools/enable_exfat.sh");
#endif
        } else {
            ESP_LOGE(TAG, "no card (%s)", esp_err_to_name(ret));
        }
        return ret;
    }

    const uint64_t bytes = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
    ESP_LOGI(TAG, "  %-12s %s", "name", s_card->cid.name);
    ESP_LOGI(TAG, "  %-12s %s", "type",
             s_card->is_mmc ? "MMC/eMMC"
                            : (s_card->ocr & SD_OCR_CCS_BIT) ? "SDHC/SDXC" : "SDSC");
    ESP_LOGI(TAG, "  %-12s %llu MB", "capacity", bytes / (1024 * 1024));
    ESP_LOGI(TAG, "  %-12s %d kHz", "speed", s_card->max_freq_khz);
    ESP_LOGI(TAG, "  %-12s %d-bit", "bus width", s_card->log_bus_width ? 4 : 1);
    return ESP_OK;
}

/* First playable file in the root directory. Not recursive, not sorted --
 * FatFs hands entries back in directory order, so "first" means first on
 * the volume. Which extensions count is decoder.c's call, not this
 * file's. */
static bool find_first_track(char *out, size_t out_len)
{
    DIR *d = opendir(SD_MOUNT);
    if (!d) {
        ESP_LOGE(TAG, "cannot open %s", SD_MOUNT);
        return false;
    }
    struct dirent *e;
    bool found = false;
    while ((e = readdir(d)) != NULL) {
        if (e->d_type == DT_DIR) continue;
        if (decoder_supports(e->d_name)) {
            snprintf(out, out_len, SD_MOUNT "/%s", e->d_name);
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

/* ------------------------------------------------------------------ */
/* Playback                                                            */
/* ------------------------------------------------------------------ */

static StreamBufferHandle_t s_pcm;
static volatile bool s_decode_done;

/* Drains the ring into I2S and nothing else, so the only thing it ever
 * blocks on is DMA. */
static void i2s_writer_task(void *arg)
{
    uint8_t *buf = heap_caps_malloc(PCM_CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!buf) { vTaskDelete(NULL); return; }

    while (1) {
        const size_t got = xStreamBufferReceive(s_pcm, buf, PCM_CHUNK_BYTES,
                                                pdMS_TO_TICKS(100));
        if (got) {
            size_t written = 0;
            i2s_channel_write(s_tx, buf, got, &written, portMAX_DELAY);
        } else if (s_decode_done && xStreamBufferIsEmpty(s_pcm)) {
            break;
        }
    }
    free(buf);
    vTaskDelete(NULL);
}

/*
 * Decode into the ring until the file runs out.
 *
 * The ID3v2 skipping and end-tag trimming that used to live here is gone:
 * minimp3_ex does it, and the esp_audio_codec parsers do their own
 * container handling. That also removed the ID3v2-footer and APEv2 bugs
 * the hand-rolled version had, because neither is now our problem.
 */
static void play_file(const char *path)
{
    decoder_t *dec = decoder_open(path);
    if (!dec) return;

    /* Both in PSRAM. The mono-to-stereo scratch is twice the decode
     * buffer and 36 KB of it as a static would be 36 KB of internal RAM
     * spent on a case most files do not hit. */
    int16_t *pcm = heap_caps_malloc(DECODER_MAX_INT16 * sizeof(int16_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *st = heap_caps_malloc(DECODER_MAX_INT16 * 2 * sizeof(int16_t),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcm || !st) {
        ESP_LOGE(TAG, "out of memory");
        free(pcm);
        free(st);
        decoder_close(dec);
        return;
    }

    uint32_t cur_rate = 0;
    int cur_chans = 0;
    int blocks = 0;

    while (1) {
        decoder_info_t info;
        const int n = decoder_read(dec, pcm, DECODER_MAX_INT16, &info);
        if (n <= 0) break;

        if ((uint32_t)info.sample_rate != cur_rate || info.channels != cur_chans) {
            ESP_LOGI(TAG, "%s: %d Hz, %d ch, %d kbps",
                     info.codec, info.sample_rate, info.channels,
                     info.bitrate_kbps);

            if (cur_rate == 0) {
                /* First block: set the rate before anything is queued,
                 * so the reconfigure never happens mid-stream. */
                ESP_ERROR_CHECK(i2s_set_rate((uint32_t)info.sample_rate));
                s_pcm = xStreamBufferCreate(PCM_RING_BYTES, PCM_CHUNK_BYTES);
                s_decode_done = false;
                xTaskCreate(i2s_writer_task, "i2s_wr", 4096, NULL, 6, NULL);
            } else if ((uint32_t)info.sample_rate != cur_rate) {
                /* i2s_channel_reconfig_std_clock() needs the channel
                 * disabled, and doing that with audio queued is an
                 * audible click. Drain first. */
                while (!xStreamBufferIsEmpty(s_pcm)) vTaskDelay(1);
                ESP_ERROR_CHECK(i2s_set_rate((uint32_t)info.sample_rate));
            }
            cur_rate = (uint32_t)info.sample_rate;
            cur_chans = info.channels;
        }

        /* The I2S slot config is stereo, so mono is duplicated into both
         * slots. Done here rather than in the backends so neither of them
         * has to know about the output format. */
        if (info.channels == 1) {
            for (int i = 0; i < n; i++) {
                st[2 * i] = st[2 * i + 1] = pcm[i];
            }
            xStreamBufferSend(s_pcm, st, (size_t)n * 2 * sizeof(int16_t),
                              portMAX_DELAY);
        } else {
            xStreamBufferSend(s_pcm, pcm, (size_t)n * sizeof(int16_t),
                              portMAX_DELAY);
        }
        blocks++;
    }

    s_decode_done = true;
    if (s_pcm) {
        while (!xStreamBufferIsEmpty(s_pcm)) vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGI(TAG, "finished, %d blocks", blocks);

    free(pcm);
    free(st);
    decoder_close(dec);
}

/* ------------------------------------------------------------------ */

void app_main(void)
{
    /* The SD drivers narrate every probe; let this file decide what is
     * worth printing. Needs CONFIG_LOG_DYNAMIC_LEVEL_CONTROL on v6. */
    esp_log_level_set("sdmmc_common", ESP_LOG_NONE);
    esp_log_level_set("sdmmc_init", ESP_LOG_NONE);
    esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_NONE);
    esp_log_level_set("SD_HOST", ESP_LOG_NONE);
    esp_log_level_set("ldo", ESP_LOG_ERROR);

    ESP_ERROR_CHECK(i2c_bus_init());
    ESP_ERROR_CHECK(io_expanders_init());
    ESP_ERROR_CHECK(backlight_init());
    ESP_ERROR_CHECK(panel_init());
    ESP_ERROR_CHECK(backlight_set(LCD_BRIGHTNESS_PERCENT));

    /* MCLK must be running before the codec's DAC comes up. */
    ESP_ERROR_CHECK(i2s_init(44100));
    ESP_ERROR_CHECK(es8388_init());

    ESP_ERROR_CHECK(sd_mount());

    char path[300];
    if (!find_first_track(path, sizeof(path))) {
        ESP_LOGE(TAG, "nothing playable in %s", SD_MOUNT);
        return;
    }
    ESP_LOGI(TAG, "playing %s", path);

    /* Cover art, if the tag carries one. Decoded and drawn before
     * playback starts so the JPEG engine is not competing with the
     * decoder for PSRAM bandwidth.
     *
     * albumart_extract() reads ID3v2 specifically, so this finds nothing
     * in a FLAC (PICTURE metadata block) or an M4A (covr atom). Those are
     * a separate parser each and are not written yet -- see README. */
    FILE *af = fopen(path, "rb");
    if (af) {
        uint8_t *jpg = NULL;
        size_t jpg_len = 0;
        if (albumart_extract(af, &jpg, &jpg_len) == ESP_OK) {
            if (albumart_show(s_panel, LCD_H_RES, LCD_V_RES, jpg, jpg_len) != ESP_OK) {
                ESP_LOGW(TAG, "cover art failed to decode");
            }
            free(jpg);
        } else {
            ESP_LOGI(TAG, "no cover art in tag");
        }
        fclose(af);
    }

    xTaskCreate(headphone_task, "hp_det", 3072, NULL, 3, NULL);

    play_file(path);

    esp_vfs_fat_sdcard_unmount(SD_MOUNT, s_card);
    ESP_LOGI(TAG, "done");
}
