/*
 * M5Stack Tab5 (ESP32-P4) -- play a chosen track, or a chosen folder,
 * from the microSD card or a USB drive.
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
 *      every card reads as absent -- identical to an empty slot. That,
 *      and the USB-A port's VBUS enable, now live in storage.c.
 *
 *   3. The ES8388 needs MCLK running before it will answer sensibly on
 *      I2S. We start the I2S channel (which drives MCLK) before writing
 *      the codec's DAC power-up registers.
 *
 * SPDX-License-Identifier: MIT
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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
#include "browser.h"
#include "decoder.h"
#include "framewalk.h"
#include "playlist.h"
#include "storage.h"
#include "touch.h"
#include "ui.h"
#include "waveform.h"

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

    ESP_RETURN_ON_ERROR(es8388_set_volume(50), TAG, "volume");   /* matches s_volume */
    ESP_LOGI(TAG, "ES8388 initialised (0x%02X)", ES8388_ADDR);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Playback                                                            */
/* ------------------------------------------------------------------ */

static StreamBufferHandle_t s_pcm;
static volatile bool s_decode_done;

/* The writer used to outlive the one and only file. Now that play_file()
 * is called once per track, the ring has to be freed at the end of each
 * one -- and freeing it while the writer is still inside
 * xStreamBufferReceive() on it is a use-after-free rather than an error.
 * So the writer says when it has gone. */
static volatile bool s_writer_done;

/* Drains the ring into I2S and nothing else, so the only thing it ever
 * blocks on is DMA. */
static void i2s_writer_task(void *arg)
{
    uint8_t *buf = heap_caps_malloc(PCM_CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!buf) { s_writer_done = true; vTaskDelete(NULL); return; }

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
    s_writer_done = true;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* Player state                                                        */
/* ------------------------------------------------------------------ */

/* Shared between the decode loop and the UI task. Deliberately plain
 * scalars rather than a mutex-guarded struct: every field is a single
 * aligned word, the UI only ever reads what the decoder writes and vice
 * versa, and a torn read here costs one frame of a slightly wrong slider
 * position. A lock would cost the decoder a blocking call per frame to
 * protect against that. */
static volatile bool     s_playing = true;
static volatile int      s_volume = 50;
static volatile uint32_t s_pos_sec;
static volatile uint32_t s_len_sec;
/* Whether a drag would do anything. Not the same question as whether the
 * length is known -- see decoder_can_seek(). */
static volatile bool     s_can_seek;
static volatile bool     s_screen_off;

/*
 * The chooser hands a path back here.
 *
 * s_pending is written by the UI task and read by the decode loop, which
 * is the one place in this file where a plain scalar is not enough: it is
 * a buffer, and a decode loop that read it half-written would open a
 * truncated path. s_pending_ready is the handshake -- written last by the
 * UI task, cleared first by the decode loop -- so the buffer is only ever
 * read between a complete write and the acknowledgement.
 */
static char              s_pending[512];
static volatile bool     s_pending_ready;

/* Set by the UI task when the chooser closes, for any reason. The decode
 * loop repaints the cover art, because the chooser drew over it and the
 * art is the one thing on screen ui_draw() does not own. */
static volatile bool     s_repaint_art;

/* Asked for by the decode loop, honoured by the UI task: the chooser is
 * opened from whichever task polls touch, so there is one writer to the
 * framebuffer. */
static volatile bool     s_open_chooser;

/* Set by the UI, consumed by the decode loop. -1 = nothing pending. Seek
 * is not implemented yet -- see README -- so the decode loop currently
 * logs and clears it. */
static volatile int      s_seek_pct = -1;

/*
 * The background envelope scan.
 *
 * A whole-song walk, so it does not run before playback -- it runs
 * alongside it, on the lowest priority task in the program, and the
 * result is drawn whenever it lands. Nothing here is animated, so a frame
 * that appears a second in is invisible; a second of silence before the
 * first note would not be.
 *
 * The task computes and does not draw. Drawing happens on the decode loop
 * for the same reason album art does: one writer to the framebuffer, no
 * lock. s_wave_ready is the handoff.
 */
static char              s_scan_path[512];
static volatile bool     s_scan_want;
static volatile bool     s_scan_abort;
static volatile bool     s_wave_ready;
static volatile bool     s_wave_framed;   /* cover present, so draw a border */
static framewalk_t       s_walk;

/* The track being played, its tags, and the filename fallback.
 *
 * s_path was a local in app_main() when there was one file. It is a
 * static now because s_display_name points into it and the UI task reads
 * that on every frame -- a local would have gone out of scope the moment
 * the second track started.
 *
 * Written by the decode loop between tracks, while the UI is drawing the
 * previous track's title. Worst case is one frame of the old name against
 * the new artwork, which is a frame, not a fault. */
static char s_path[512];
static id3_tags_t s_tags;
static const char *s_display_name = "";

/* Read the tags and put the cover on screen. Was inline in app_main();
 * it runs once per track now.
 *
 * Called from the decode loop rather than the UI task deliberately: it
 * fopen()s the track and pushes a JPEG through the hardware codec, and
 * the UI task has 4 KB of stack and a 20 ms period. Doing it here costs
 * the ring a fraction of its 0.37 s of slack instead. */
static void load_track_visuals(const char *path)
{
    memset(&s_tags, 0, sizeof(s_tags));

    s_display_name = strrchr(path, '/');
    s_display_name = s_display_name ? s_display_name + 1 : path;

    FILE *af = fopen(path, "rb");
    if (!af) return;

    if (id3_read_tags(af, &s_tags) == ESP_OK) {
        ESP_LOGI(TAG, "tags: \"%s\" / \"%s\" / \"%s\"",
                 s_tags.title, s_tags.artist, s_tags.album);
    } else {
        ESP_LOGI(TAG, "no ID3 text frames; showing the filename");
    }

    /* albumart_extract() reads ID3v2 specifically, so this finds nothing
     * in a FLAC (PICTURE metadata block) or an M4A (covr atom). Those are
     * a separate parser each and are not written yet -- see README.
     *
     * The area above the bar is cleared either way. Without that, a track
     * with no art keeps the previous track's cover, which reads as the
     * player having ignored the choice. */
    ui_clear_art();

    bool have_art = false;
    uint8_t *jpg = NULL;
    size_t jpg_len = 0;
    if (albumart_extract(af, &jpg, &jpg_len) == ESP_OK) {
        /* The artwork area, not the panel: everything below belongs to
         * the transport bar and must not be cleared from here. */
        const esp_err_t serr = albumart_show(s_panel, LCD_H_RES,
                                             LCD_V_RES - UI_BAR_H,
                                             jpg, jpg_len);
        if (serr != ESP_OK) {
            ESP_LOGW(TAG, "cover art failed to decode (%s)",
                     esp_err_to_name(serr));
        } else {
            have_art = true;
        }
        free(jpg);
    } else {
        ESP_LOGI(TAG, "no cover art in tag");
    }
    fclose(af);

    /* After the art, not before -- this snapshots what the finger bubble
     * has to put back. */
    ui_capture_background();

    /* Kick the envelope scan for this track. Cancels whatever the scan
     * task was doing first: on a fast run through a folder the previous
     * walk is still going, and finishing it would draw the wrong track's
     * envelope over the right track's cover. */
    s_scan_abort = true;
    s_wave_ready = false;
    s_wave_framed = have_art;
    snprintf(s_scan_path, sizeof(s_scan_path), "%s", path);
    s_scan_want = true;
}

/*
 * Walks the current track and stops when asked.
 *
 * Its own FILE*, deliberately: the decoder holds one and this seeks to
 * EOF and back. Sharing it would be a seek war with the thing producing
 * the audio.
 */
static void scan_task(void *arg)
{
    (void)arg;
    char path[512];

    while (1) {
        if (!s_scan_want) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        s_scan_want = false;
        s_scan_abort = false;
        snprintf(path, sizeof(path), "%s", s_scan_path);

        FILE *f = fopen(path, "rb");
        if (!f) continue;

        /* Decline before reading anything. Walking a format with no
         * parser here is a whole file off the card in exchange for a log
         * line saying it found nothing -- and it is contention with the
         * decoder reading the same card for the same track. */
        if (!framewalk_supports(f)) {
            ESP_LOGI(TAG, "no frame walker for this format; no envelope");
            fclose(f);
            continue;
        }

        /* The frame layout uses four sides of WAVE_INNER; the fill layout
         * uses one column per pixel. Asking for the larger of the two
         * costs nothing and means the mode can change without a rescan. */
        /* One column per pixel of panel width, both shapes -- the frame
         * is the same envelope with a square skipped, not a different
         * layout, so it wants the same column count. */
        const int cols = LCD_H_RES;

        const esp_err_t err = framewalk_scan(f, cols, &s_scan_abort, &s_walk);
        fclose(f);

        /* Loud, because every way this can fail is silent otherwise: an
         * aborted scan, a format with no per-frame loudness, and a walk
         * that found no frames at all all end with simply no envelope on
         * screen and nothing said about it. */
        if (err != ESP_OK) {
            ESP_LOGI(TAG, "scan cancelled");
        } else if (s_scan_abort) {
            ESP_LOGI(TAG, "scan finished but the track moved on");
        } else if (!s_walk.has_levels) {
            ESP_LOGI(TAG, "no per-frame loudness in this format; no envelope");
        } else if (!s_walk.frames) {
            ESP_LOGW(TAG, "walk found no frames; no envelope");
        } else {
            ESP_LOGI(TAG, "envelope ready: %d columns, %s",
                     s_walk.columns, s_wave_framed ? "framed" : "fill");
            s_wave_ready = true;
        }
    }
}

/* Hand a chosen path to the decode loop. */
static void request_track(const char *path)
{
    snprintf(s_pending, sizeof(s_pending), "%s", path);
    s_pending_ready = true;
}

static void ui_task(void *arg)
{
    ui_state_t st;

    while (1) {
        int bx = 0, by = 0;
        const bool bdown = touch_get(&bx, &by);

        /* The chooser, when it is up, is the whole screen and the whole
         * interaction. It is driven from here rather than from its own
         * task so there is exactly one writer to the framebuffer -- the
         * same reason ui_draw() and albumart_show() never overlap. */
        if (s_open_chooser) {
            s_open_chooser = false;
            browser_open(s_path);
        }

        if (browser_is_open()) {
            const browser_result_t r = browser_touch(bdown, bx, by);
            switch (r.kind) {
            case BROWSER_PLAY_FILE:
                /* The folder the track came from becomes the list, so
                 * "play this one" and "then carry on" are one choice
                 * rather than two. */
                {
                    char dir[512];
                    snprintf(dir, sizeof(dir), "%s", r.path);
                    char *slash = strrchr(dir, '/');
                    if (slash && slash != dir) *slash = '\0';
                    if (playlist_load_dir(dir) == ESP_OK) {
                        playlist_set_current(playlist_index_of(r.path));
                    }
                    request_track(r.path);
                }
                browser_close();
                s_repaint_art = true;
                break;
            case BROWSER_PLAY_FOLDER:
                if (playlist_load_dir(r.path) == ESP_OK && playlist_count() > 0) {
                    playlist_set_current(0);
                    request_track(playlist_path(0));
                } else {
                    ESP_LOGW(TAG, "nothing playable in %s", r.path);
                }
                browser_close();
                s_repaint_art = true;
                break;
            case BROWSER_CANCELLED:
                browser_close();
                s_repaint_art = true;
                break;
            default:
                break;
            }
            browser_draw();
            vTaskDelay(pdMS_TO_TICKS(bdown ? 20 : 100));
            continue;
        }

        st.title = s_tags.title[0] ? s_tags.title : s_display_name;
        st.artist = s_tags.artist;
        st.album = s_tags.album;
        st.playing = s_playing;
        st.volume = s_volume;
        st.pos_sec = s_pos_sec;
        st.len_sec = s_len_sec;
        st.can_seek = s_can_seek;
        st.screen_off = s_screen_off;

        const bool down = bdown;
        const ui_action_t act = ui_touch(&st, down, bx, by);

        switch (act.kind) {
        case UI_ACTION_PLAY_PAUSE:
            s_playing = !s_playing;
            break;
        case UI_ACTION_VOLUME:
            s_volume = act.value;
            es8388_set_volume((uint8_t)act.value);
            break;
        case UI_ACTION_SEEK:
            s_seek_pct = act.value;
            break;
        case UI_ACTION_CHOOSE_FILE:
            s_open_chooser = true;
            break;
        case UI_ACTION_SCREEN_OFF:
            s_screen_off = true;
            backlight_set(0);
            break;
        case UI_ACTION_SCREEN_ON:
            s_screen_off = false;
            backlight_set(LCD_BRIGHTNESS_PERCENT);
            break;
        default:
            break;
        }

        /* 50 Hz while a finger is down so drags feel attached to it, 10 Hz
         * otherwise. Polling the panel at 50 Hz constantly would put a
         * needless I2C transaction between the decoder and the SD card
         * forty times a second for nothing. */
        st.playing = s_playing;
        st.volume = s_volume;
        st.len_sec = s_len_sec;
        st.can_seek = s_can_seek;
        st.screen_off = s_screen_off;
        ui_draw(&st);
        vTaskDelay(pdMS_TO_TICKS(down ? 20 : 100));
    }
}

/*
 * Decode into the ring until the file runs out.
 *
 * The ID3v2 skipping and end-tag trimming that used to live here is gone:
 * minimp3_ex does it, and the esp_audio_codec parsers do their own
 * container handling. That also removed the ID3v2-footer and APEv2 bugs
 * the hand-rolled version had, because neither is now our problem.
 */
/* Why the track ended. The caller needs to tell "the file finished, go on
 * to the next one" from "something else was chosen, do not". */
typedef enum {
    TRACK_ENDED = 0,    /* end of file, or an unreadable one */
    TRACK_INTERRUPTED,  /* the chooser picked something else */
    TRACK_MEDIA_GONE,   /* the card or drive was pulled */
} track_end_t;

static track_end_t play_file(const char *path)
{
    const storage_id_t vol = storage_of_path(path);

    decoder_t *dec = decoder_open(path);
    if (!dec) return TRACK_ENDED;

    /* Tell storage.c not to unmount underneath this FILE*. It will still
     * mark the volume absent the moment the card stops answering -- the
     * loop below watches for that -- but the unmount itself waits until
     * the decoder is closed. */
    storage_hold(vol);

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
        storage_hold(STORAGE_COUNT);
        return TRACK_ENDED;
    }

    uint32_t cur_rate = 0;
    int cur_chans = 0;
    int blocks = 0;
    uint64_t frames_out = 0;
    track_end_t why = TRACK_ENDED;

    load_track_visuals(path);

    /* Consume any repaint the chooser left pending. It closed a moment
     * ago and set the flag; the call above has just drawn this track's
     * cover, so the flag is already satisfied.
     *
     * Without this the loop's first pass ran load_track_visuals() a
     * second time, ~50 ms later -- visible in the log as a duplicated
     * pair of "no ID3 text frames" / "no cover art" lines per track. The
     * d001 guard does not catch it: that one skips the repaint when a
     * track change is still queued, and by this point the change has
     * happened and s_pending_ready is back to false.
     *
     * Cheap today, load-bearing once the waveform scan hangs off this
     * function: two scans of a whole song per track change. */
    s_repaint_art = false;

    /* Before the first block, so the bar has a scale to draw against from
     * the very first repaint rather than snapping into place a frame in. */
    s_pos_sec = 0;
    s_can_seek = decoder_can_seek(dec);
    s_len_sec = decoder_duration_sec(dec);
    if (s_len_sec == 0) {
        ESP_LOGI(TAG, "no duration available; seek bar will stay empty");
    }

    while (1) {
        /* Pause stalls the decoder, not the writer: the ring drains to
         * DMA and then i2s_writer_task blocks on an empty buffer, which
         * is silence without disabling the channel. Disabling it would
         * be an audible click on every press. */
        /* The chooser drew over the artwork and has just closed. Repaint
         * from here rather than from the UI task -- see
         * load_track_visuals(). Ahead of the pause wait, because a
         * chooser opened and dismissed while paused would otherwise
         * leave the listing on screen until play resumed. */
        /* Not when a track change is already queued. The chooser sets
         * both flags on the way out, and the repaint would decode this
         * track's cover -- a 130 KB PNG -- milliseconds before play_file()
         * exits and the next track decodes its own. One full decode
         * thrown away on every switch.
         *
         * The cancel case still repaints: nothing is pending there, which
         * is exactly what distinguishes it. */
        if (s_repaint_art && !s_pending_ready) {
            s_repaint_art = false;
            load_track_visuals(path);
        }

        /* The scan landed. Drawn here rather than on the scan task so
         * there is one writer to the framebuffer. */
        if (s_wave_ready) {
            s_wave_ready = false;
            waveform_draw(&s_walk, s_wave_framed ? WAVE_FRAME : WAVE_FILL,
                          LCD_V_RES - UI_BAR_H);
            /* The bubble's saved strip is now stale -- it was captured
             * before the envelope existed. */
            ui_capture_background();

            /* The walk counted frames on the way past. For a format that
             * states its own length this is redundant and is not used;
             * for raw ADTS, AMR and a Xing-less MP3 it is the only answer
             * there is, and the bar goes from empty to filled mid-track
             * rather than staying empty for the whole song. */
            if (!s_len_sec && s_walk.sec) {
                s_len_sec = s_walk.sec;
                ESP_LOGI(TAG, "duration from frame walk: %" PRIu32 "s",
                         s_len_sec);
            }
        }

        while (!s_playing && !s_pending_ready) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        /* A choice made while paused has to end the track, or the new
         * file waits behind a pause the user has already moved on from. */
        if (s_pending_ready) {
            why = TRACK_INTERRUPTED;
            break;
        }

        /* The volume this file is on stopped answering. Nothing further
         * will read; stop before decoder_read() turns it into a stream of
         * short reads and a log line per frame. */
        if (vol < STORAGE_COUNT && !storage_present(vol)) {
            ESP_LOGW(TAG, "media removed; stopping playback");
            why = TRACK_MEDIA_GONE;
            break;
        }


        if (s_seek_pct >= 0) {
            const int pct = s_seek_pct;
            s_seek_pct = -1;

            if (s_len_sec == 0) {
                ESP_LOGI(TAG, "seek ignored: no duration for this format");
            } else {
                const uint32_t target = (uint32_t)((uint64_t)s_len_sec * pct / 100);
                const esp_err_t sr = decoder_seek_sec(dec, target);
                if (sr == ESP_ERR_NOT_SUPPORTED) {
                    ESP_LOGI(TAG, "seek ignored: %s cannot seek", "this backend");
                } else if (sr != ESP_OK) {
                    ESP_LOGW(TAG, "seek to %" PRIu32 "s failed", target);
                } else {
                    /* Drop what is already queued. Without this the ring
                     * plays out ~0.37 s of the old position after the
                     * jump, which sounds like the seek was ignored and
                     * then took effect late. */
                    if (s_pcm) xStreamBufferReset(s_pcm);

                    /* Re-anchor the position counter, or the clock counts
                     * on from where it was rather than from the new
                     * point. */
                    frames_out = (uint64_t)target * (cur_rate ? cur_rate : 1);
                    s_pos_sec = target;
                    ESP_LOGI(TAG, "seek to %" PRIu32 "s", target);
                }
            }
        }

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
                s_writer_done = false;
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
        /* Position from samples decoded, not from bytes read: with VBR
         * the two disagree, and this is the number the slider shows. */
        frames_out += (uint64_t)(n / (info.channels > 0 ? info.channels : 1));
        if (cur_rate) s_pos_sec = (uint32_t)(frames_out / cur_rate);

        blocks++;
    }

    s_decode_done = true;
    if (s_pcm) {
        /* Interrupted rather than finished: what is queued is the old
         * track, and playing 0.37 s of it after the new one was chosen
         * sounds like the tap was ignored. Dropped for the same reason
         * the ring is reset on a seek.
         *
         * A track that ended on its own drains, because those last
         * fractions of a second are the end of the song. */
        if (why == TRACK_ENDED) {
            while (!xStreamBufferIsEmpty(s_pcm)) vTaskDelay(pdMS_TO_TICKS(10));
        } else {
            xStreamBufferReset(s_pcm);
        }

        /* Wait for the writer to leave the buffer before deleting it.
         * With one file this task never came back here at all; with a
         * playlist, freeing the ring out from under a blocked
         * xStreamBufferReceive() is a use-after-free once per track. */
        while (!s_writer_done) vTaskDelay(pdMS_TO_TICKS(10));
        vStreamBufferDelete(s_pcm);
        s_pcm = NULL;
    }
    /* Why, not just that. "finished" on a track the user skipped out of
     * reads as the skip having been ignored until the next line arrives. */
    ESP_LOGI(TAG, "%s, %d blocks",
             why == TRACK_INTERRUPTED ? "interrupted"
             : why == TRACK_MEDIA_GONE ? "media gone"
                                       : "finished", blocks);

    free(pcm);
    free(st);
    decoder_close(dec);
    storage_hold(STORAGE_COUNT);

    s_pos_sec = 0;
    return why;
}

/* ------------------------------------------------------------------ */

/*
 * Pick something to start with, without making the user choose first.
 *
 * The first playable file on the first mounted volume, and its folder
 * becomes the list -- so a card with an album on it plays the album, and
 * the chooser is for changing your mind rather than for getting started.
 * Not recursive: a card whose root is nothing but folders opens the
 * chooser instead, which is the honest answer to "what should I play"
 * when there is no file to pick.
 */
static bool autostart(char *out, size_t out_len)
{
    for (int v = 0; v < STORAGE_COUNT; v++) {
        if (!storage_present((storage_id_t)v)) continue;
        const char *root = storage_mount_path((storage_id_t)v);
        if (playlist_load_dir(root) != ESP_OK) continue;
        if (playlist_count() == 0) continue;
        playlist_set_current(0);
        snprintf(out, out_len, "%s", playlist_path(0));
        return true;
    }
    return false;
}

/*
 * One track after another, forever.
 *
 * This replaced a single play_file() call. The shape is: play what is
 * queued, then ask the playlist what is next; when nothing is next, put
 * the chooser up and wait for a tap rather than returning from app_main()
 * and leaving a lit screen attached to a dead task.
 */
static void player_loop(void)
{
    bool have = autostart(s_path, sizeof(s_path));
    if (!have) {
        ESP_LOGI(TAG, "nothing to play; opening the chooser");
        s_open_chooser = true;
    }

    while (1) {
        if (s_pending_ready) {
            /* Clear the flag before reading the buffer, so a second
             * choice made during the copy is not lost silently -- it
             * simply sets the flag again and is picked up next time
             * round. */
            s_pending_ready = false;
            snprintf(s_path, sizeof(s_path), "%s", s_pending);
            have = true;
        }

        if (!have) {
            /* Idle: no decode loop is running, so the repaint that
             * normally happens there has to happen here. Otherwise a
             * chooser dismissed with nothing playing leaves its listing
             * above the bar until something is chosen. */
            if (s_repaint_art) {
                s_repaint_art = false;
                if (s_path[0]) load_track_visuals(s_path);
                else           ui_clear_art();
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        ESP_LOGI(TAG, "playing %s", s_path);
        const track_end_t why = play_file(s_path);
        have = false;

        if (why == TRACK_INTERRUPTED) continue;    /* s_pending has the next */

        if (why == TRACK_MEDIA_GONE) {
            /* The list points at paths on a volume that is no longer
             * there. Keeping it would offer a next track that cannot be
             * opened. */
            playlist_clear();
            s_open_chooser = true;
            continue;
        }

        const char *next = playlist_next(browser_order());
        if (next) {
            snprintf(s_path, sizeof(s_path), "%s", next);
            have = true;
            continue;
        }

        /* End of the folder, or single-track mode. Nothing is playing and
         * nothing is queued, so offer the chooser rather than sit on a
         * finished track. */
        ESP_LOGI(TAG, "end of %s", playlist_dir());
        s_open_chooser = true;
    }
}

void app_main(void)
{
    /* The SD drivers narrate every probe; let this file decide what is
     * worth printing. Needs CONFIG_LOG_DYNAMIC_LEVEL_CONTROL on v6.
     *
     * The USB stack is the same problem with more tasks: a drive being
     * enumerated logs a descriptor dump per device at info level. */
    esp_log_level_set("sdmmc_common", ESP_LOG_NONE);
    esp_log_level_set("sdmmc_init", ESP_LOG_NONE);
    esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_NONE);
    esp_log_level_set("SD_HOST", ESP_LOG_NONE);
    esp_log_level_set("ldo", ESP_LOG_ERROR);
    esp_log_level_set("USBH", ESP_LOG_WARN);
    esp_log_level_set("Hub", ESP_LOG_WARN);

    ESP_ERROR_CHECK(i2c_bus_init());
    ESP_ERROR_CHECK(io_expanders_init());
    ESP_ERROR_CHECK(backlight_init());
    ESP_ERROR_CHECK(panel_init());
    ESP_ERROR_CHECK(backlight_set(LCD_BRIGHTNESS_PERCENT));

    /* MCLK must be running before the codec's DAC comes up. */
    ESP_ERROR_CHECK(i2s_init(44100));
    ESP_ERROR_CHECK(es8388_init());

    /* Both volumes, and the poll task that keeps them up to date. Not
     * ESP_ERROR_CHECK'd on the media itself: an empty slot and an empty
     * port are a normal way to boot now, and the chooser says so on
     * screen. */
    ESP_ERROR_CHECK(storage_init(s_exp2));

    xTaskCreate(headphone_task, "hp_det", 3072, NULL, 3, NULL);

    /* Lowest priority in the program. It reads a whole file off the same
     * card the decoder is reading, and the decoder winning every time is
     * the correct outcome. */
    xTaskCreate(scan_task, "wave", 4096, NULL, 1, NULL);

    /* Touch after the panel, always: TP_RST and LCD_RST are released by
     * the same expander write, so before panel_init() there is nothing on
     * the bus to talk to. */
    if (touch_init(s_i2c_bus, LCD_H_RES, LCD_V_RES) != ESP_OK) {
        ESP_LOGW(TAG, "no touch -- playing through with no controls");
    }
    ESP_ERROR_CHECK(ui_init(s_panel, LCD_H_RES, LCD_V_RES));
    ui_capture_background();
    /* 4 KB was enough when this task only drew the bar. The chooser runs
     * on it too now, and that reaches opendir()/readdir() through FatFs
     * and carries a couple of 512-byte path buffers on the way -- so the
     * old size overflowed on the first folder with a long name in it. */
    xTaskCreate(ui_task, "ui", 8192, NULL, 4, NULL);

    /* Does not return. The volumes are never unmounted from here any
     * more; storage.c owns that, and it unmounts on removal rather than
     * on the end of a track. */
    player_loop();
}
