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
 *      I2S. The I2S channel (which drives MCLK) is started before the
 *      codec's DAC power-up registers are written. That, the codec, the
 *      amp enable and the headphone-detect line now live in
 *      audio_out.c.
 *
 * SPDX-License-Identifier: MIT
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_timer.h"
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
#include "audio_out.h"
#include "battery.h"
#include "covertag.h"
#include "heapcheck.h"
#include "mediacache.h"
#include "browser.h"
#include "decoder.h"
#include "framewalk.h"
#include "replaygain.h"
#include "hid.h"
#include "playlist.h"
#include "settings.h"
#include "storage.h"
#include "storage_io.h"
#include "touch.h"
#include "uac.h"
#include "ui.h"
#include "usbhost.h"
#include "waveform.h"

static const char *TAG = "tab5_mp3";

/*
 * The envelope scan, off unless -DWAVEFORM=1.
 *
 * It is a whole-file read that produces a frame count, a duration and a
 * loudness envelope. On the files that motivated it -- Xing-less MP3 --
 * decoder_open() has already read the whole file to build minimp3's seek
 * index by the time this runs, so the same 8.7 MB goes past twice for two
 * answers that overlap. Measured on an 8.7 MB track: 15.4 s in the open,
 * then a 273 s envelope from a second pass.
 *
 * Gated rather than deleted, and gated with a plain `if` rather than
 * `#if`, so every line below still compiles and is still type-checked
 * against the rest of the file. Dead code that stops building is dead
 * code that cannot be switched back on to compare against.
 */
#ifndef WAVEFORM_SCAN
#define WAVEFORM_SCAN   (0)
#endif

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

/* ---- Display: ST7121 MIPI-DSI, portrait native ---- */
#define LCD_H_RES               (720)
#define LCD_V_RES               (1280)
#define LCD_BITS_PER_PIXEL      (24)
#define DSI_DATA_LANES          (2)
#define DSI_LANE_RATE_MBPS      (965)
/*
 * Back to 70 MHz, which is ~57.3 Hz. 29 MHz blanked the panel.
 *
 * The reasoning that got it to 29 still stands as far as it goes. The
 * DPI peripheral reads the framebuffer out of PSRAM continuously and
 * cannot wait; miss a line and the DSI bridge underruns, the panel goes
 * cyan for a frame, and the ISR in esp_lcd/dsi/esp_lcd_panel_dpi.c
 * prints
 *
 *   can't fetch data from external memory fast enough, underrun happens
 *
 * Nothing here can yield to that fetch -- it is a DMA master on the AXI
 * bus rather than a task -- so the only lever is how much it asks for,
 * and at 1.84 MB a frame the refresh rate IS the bandwidth:
 *
 *   total = (720+2+40+40) x (1280+20+24+200) = 802 x 1524 = 1,222,248
 *   70 MHz -> 57.3 Hz -> 105 MB/s
 *   50 MHz -> 40.9 Hz ->  75 MB/s
 *   29 MHz -> 23.7 Hz ->  44 MB/s
 *
 * What that reasoning left out is that the rate is not ours to choose
 * freely. The ST7121 runs its own timing generator locked to the
 * incoming VSYNC, and below its lock range it stops driving the glass
 * rather than degrading: backlight on, esp_lcd_panel_init() returning
 * ESP_OK, the log clean to the last line, and a black screen. There is
 * no error anywhere, because from the SoC's side nothing failed.
 *
 * 24 Hz is under that floor. Where the floor actually is has not been
 * measured -- 70 is the only rate this panel is known to hold, which is
 * why it is what this reverts to rather than something in between.
 *
 * To find it, walk down one step at a time and reflash at each:
 *
 *   60 -> 49.1 Hz -> 90 MB/s
 *   55 -> 45.0 Hz -> 83 MB/s
 *   50 -> 40.9 Hz -> 75 MB/s
 *   45 -> 36.8 Hz -> 68 MB/s
 *
 * and take one step back from wherever it goes dark. Two things to know
 * while doing it:
 *
 *   - The number here is not the number on the wire.
 *     dpi_panel_create() divides the source clock by an INTEGER, via
 *     mipi_dsi_hal_host_dpi_calculate_divider(), so the rate is the
 *     nearest the divider allows and not what is written above. Log the
 *     divider before trusting any of the figures in this comment.
 *   - DSI_LANE_RATE_MBPS is deliberately not moved with it. The lane
 *     rate is the wire, not the memory, and costs no PSRAM bandwidth; it
 *     only has to stay above what the pixel clock demands, which it does
 *     by an increasing margin as this comes down. If a low rate ever
 *     gives a sheared or misaligned picture rather than a black one,
 *     that is the mismatch to go and look at, against a documented floor
 *     of 480.
 *
 * And the honest conclusion from the attempt: this is the smaller and
 * riskier of the two levers. The clock buys tens of MB/s and can blank
 * the panel. ui_draw() spends around 160 MB/s repainting the transport
 * bar at 50 Hz under a finger -- a full 560-row clear, a 720-column
 * envelope loop, a 560-row blit, a bubble memcpy and a second blit --
 * and shrinking that carries no risk of anything going dark. Do that
 * first.
 */
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
 * 10 MB is ~59 s of 44.1 kHz stereo. That is far more than the worst
 * observed stall -- decoder_read has been seen blocked for 11.4 s and
 * 11.7 s -- and the excess is deliberate: the point of this size is to
 * settle whether a ring can hide these pauses at all, not to be the
 * right size. A ring that does not span the stall does not hide the
 * stall, and 64 KB (0.37 s) tells you nothing about whether 12 s would
 * have been enough.
 *
 * So this is a measurement, and 10 MB is the setting that cannot be
 * blamed for an inconclusive result. If the pauses stop, the number
 * comes down until they start again and the answer is one notch above
 * that. If they do NOT stop at 59 s of buffer, the ring was never the
 * lever and this whole approach is finished -- which is worth knowing in
 * one flash rather than three.
 *
 * PSRAM is 32 MB, so this is under a third of it, against a cover cache
 * and a 720x720 RGB565 enlargement that are the other large tenants.
 *
 * PATCH 06 DID THIS BEFORE AND WAS REVERTED. It moved the ring to 256 KB
 * in PSRAM via xStreamBufferCreateWithCaps(), and was backed out during
 * a heap corruption hunt -- not because the larger ring was shown to be
 * the fault, but because it was the one thing about this allocation that
 * had changed, and a variable that cannot be reasoned about should be
 * removed before it is defended.
 *
 * So this deliberately does not reinstate that call. The storage is
 * allocated once at boot and never freed, and the buffer is created
 * static on top of it:
 *
 *   - xStreamBufferCreateStatic() allocates nothing, so there is no
 *     allocator to pair with a matching free.
 *   - vStreamBufferDelete() on a static buffer frees nothing -- it
 *     checks the statically-allocated flag and returns -- so the
 *     existing per-track delete stays exactly as it is, with all of its
 *     reasoning about handle ordering intact.
 *   - The PSRAM block is allocated in app_main() and outlives every
 *     track, so nothing about the per-track path allocates or frees at
 *     all.
 *
 * That is a different shape from what was reverted, and it is a smaller
 * one: the reverted version created and destroyed a PSRAM allocation on
 * every track boundary. This creates none.
 *
 * What it does not do is fix anything. An eleven-second read is still an
 * eleven-second read; this buys enough slack to not hear it, which is
 * worth having and is not the same as the card working.
 */
#define PCM_RING_BYTES          (10 * 1024 * 1024)
#define PCM_CHUNK_BYTES         (4 * 1024)

/*
 * How long the decode loop will sit in one xStreamBufferSend() before
 * coming up for air to look at the controls.
 *
 * This, not the ring size, is now the worst-case delay between pressing
 * something and the decode loop noticing. 20 ms is one decoded MP3 frame
 * at 44.1 kHz, so a full ring costs at most one frame of latency per
 * check rather than however many seconds the ring happens to hold.
 */
#define SEND_SLICE_MS           (20)

/*
 * A decode-loop phase slower than this gets a line.
 *
 * 400 ms is well past anything healthy -- a block is ~26 ms of audio --
 * and well short of a delay anyone would notice as a button not
 * responding, so it catches the cause before the symptom.
 */
#define LOOP_STALL_MS           (400)

/* Input buffering now lives inside decoder.c -- minimp3_ex does its own
 * over its IO callbacks, and the esp_audio_codec path keeps a sliding
 * window. Only the PCM side is sized here. */

static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_exp1, s_exp2;

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
/* Playback                                                            */
/* ------------------------------------------------------------------ */

static StreamBufferHandle_t s_pcm;

/*
 * Ring occupancy, 0-100, or -1 when there is no ring.
 *
 * A published NUMBER rather than a shared handle, and that distinction
 * is the whole point of it.
 *
 * media_task needs to know how full the ring is, to decide whether it
 * can afford to read. The obvious way to tell it is to let it call
 * xStreamBufferBytesAvailable(s_pcm) -- which is what the first version
 * did, and which is a use-after-free waiting to happen: the decode loop
 * deletes the ring at the end of every track, and between the delete and
 * the s_pcm = NULL that follows it, any other task holding that handle
 * is reading freed PSRAM. A `if (!s_pcm) return` guard does not fix it
 * either; the pointer can be loaded before the check and used after the
 * free.
 *
 * It crashed exactly where that predicts: pressing next while the frame
 * walk was running, so media_task was polling occupancy every 100 ms and
 * eventually landed inside the window. The symptom was a TLSF assert on
 * a later, innocent free, because what the read corrupts is the heap
 * metadata rather than anything of ours.
 *
 * So the handle stays private to the decode loop and the writer, and the
 * decode loop publishes an int. An int cannot dangle. -1 means "no ring"
 * and is written before the ring goes away, not after.
 */
static volatile int s_ring_pct = -1;

/*
 * What the decode loop has produced, published for the writer.
 *
 * The position shown is decoded-minus-queued, and only the writer runs
 * for the whole time the ring is draining -- the decode loop exits as
 * soon as the file is finished, which at 10 MB is up to a minute before
 * the last sample is heard. Computing the position in the decode loop
 * therefore froze it at (length - ring contents) for the rest of the
 * track: the remaining-time display stuck at -00:59 and stayed there.
 *
 * Three plain integers rather than a handle, for the reason above: an
 * int cannot dangle. The writer holds the ring anyway, so it is the one
 * task that can ask how much is still queued at any moment.
 */
static volatile uint64_t s_frames_out;
static volatile uint32_t s_frames_rate;
static volatile uint32_t s_frames_chans = 2;

/*
 * The ring's memory, allocated once and never freed.
 *
 * Separate from the handle because the handle is per track and this is
 * not. xStreamBufferCreateStatic() wants storage one byte larger than
 * the usable size -- the implementation keeps head and tail distinct
 * that way -- and it is the caller's job to know that.
 */
static StaticStreamBuffer_t s_pcm_struct;
static uint8_t             *s_pcm_store;
static volatile bool s_decode_done;

/* The writer used to outlive the one and only file. Now that play_file()
 * is called once per track, the ring has to be freed at the end of each
 * one -- and freeing it while the writer is still inside
 * xStreamBufferReceive() on it is a use-after-free rather than an error.
 * So the writer says when it has gone. */
static volatile bool s_writer_done;

/*
 * Set by play_file() before it waits for the writer, and the reason the
 * pause gate below is not simply "if (!s_playing)".
 *
 * Teardown ends with two spins -- drain the ring on TRACK_ENDED, then
 * wait for s_writer_done -- and both are waiting on the writer to move.
 * A writer parked on a pause never does, so pausing at the wrong moment
 * would hang the decode loop against a task that is deliberately not
 * running, with the seek and next-track paths gone with it. Being
 * paused during teardown means the last few seconds of a finished track
 * play out; deadlocking means the player stops answering.
 *
 * Declared with the other two rather than near the flag it overrides,
 * because these three are one handshake and reading them apart is how
 * the deadlock got written in the first place.
 */
static volatile bool s_writer_stop;

/* Tentative declaration. The definition, with the rest of the shared
 * player state, is further down; the writer needs the name here and the
 * writer comes first in the file. Kept static rather than extern so it
 * stays internal to this translation unit like everything around it. */
static volatile bool s_playing;

static void ring_publish(void);

/* Drains the ring into I2S and nothing else, so the only thing it ever
 * blocks on is DMA. */
static void i2s_writer_task(void *arg)
{
    uint8_t *buf = heap_caps_malloc(PCM_CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!buf) { s_writer_done = true; vTaskDelete(NULL); return; }

    while (1) {
        /*
         * Pause stops the writer, which is the only place it can stop.
         *
         * It used to stop the decode loop instead -- see the comment on
         * PCM_RING_BYTES. That was right when the ring held 0.37 s: stall
         * the producer, the consumer empties in a third of a second, done.
         * At 10 MB the ring holds 59 seconds, and stalling the producer
         * left the writer to play all of it out. Pause fell silent up to
         * a minute after the press, with audio_out_set_idle() cutting the
         * amp somewhere in the middle of that, so the symptom read as
         * "it started playing while paused" rather than as a late pause.
         *
         * The ring got 160x bigger and this was not revisited. That is
         * what makes it worth stating rather than just fixing: pause
         * against a buffer is only immediate at the end of the buffer the
         * listener actually hears, and the correct end does not depend on
         * the size. Whatever PCM_RING_BYTES becomes, this stays right.
         *
         * The ring is deliberately NOT drained. Its contents are still
         * the correct next samples, so resume is instantaneous and costs
         * the card nothing -- which on a card that stalls decoder_read()
         * for five seconds at a time is the difference between resuming
         * and resuming into the next stall.
         *
         * 20 ms rather than the 50 ms the decode loop used. That was the
         * latency of noticing an unpause on a path that then had to refill
         * a ring; this is the latency of the first sample after one, and
         * 50 ms of it is audible as a lag on the button.
         */
        if (!s_playing && !s_writer_stop) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        const size_t got = xStreamBufferReceive(s_pcm, buf, PCM_CHUNK_BYTES,
                                                pdMS_TO_TICKS(100));
        ring_publish();

        if (got) {
            audio_out_write(buf, got);
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
static volatile bool     s_playing = true;   /* declared above the writer */
static volatile int      s_volume = 50;
static volatile uint32_t s_pos_sec;
static volatile uint32_t s_len_sec;
/* Whether a drag would do anything. Not the same question as whether the
 * length is known -- see decoder_can_seek(). */
static volatile bool     s_can_seek;
static volatile bool     s_screen_off;

/*
 * A press from the headset's inline remote, waiting for the UI task.
 *
 * hid.c calls back on its own polling task, which owns a live URB and
 * must not block, so it publishes and returns -- the same rule as
 * s_ring_pct. The UI task picks this up and pushes it through the SAME
 * switch the on-screen buttons go through, so the two sources cannot
 * drift in behaviour and both log the same line.
 *
 * A single slot, and a second press before the first is serviced
 * overwrites it. The UI polls at 20-50 Hz and no thumb produces two
 * presses inside 20 ms; a queue here would buy nothing and would let a
 * stuck button build a backlog that plays out after it is released.
 */
static volatile int s_hid_action = -1;   /* a ui_action_kind_t, or -1 */
static volatile int s_hid_value;

static void hid_button(hid_button_t button)
{
    int vol;
    switch (button) {
    case HID_BTN_MUTE:
        s_hid_action = UI_ACTION_MUTE;
        break;
    case HID_BTN_VOL_UP:
    case HID_BTN_VOL_DOWN:
        /* Five points a press. The on-screen slider is 528 px for 100
         * points, so a step is about the smallest move a finger makes on
         * it -- the two controls should not disagree about what "a bit
         * louder" means. */
        vol = s_volume + (button == HID_BTN_VOL_UP ? 5 : -5);
        if (vol < 0) vol = 0;
        if (vol > 100) vol = 100;
        s_hid_value = vol;
        s_hid_action = UI_ACTION_VOLUME;
        break;
    case HID_BTN_MIC_MUTE:
        /* There is no microphone in this program -- uac.c does not open
         * the RX interface. Logged by hid.c and dropped here rather than
         * quietly mapped onto something else, because a mic-mute key
         * that pauses the music is worse than one that does nothing. */
        break;
    }
}

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
 * When the outstanding seek was asked for, and by what.
 *
 * A seek is requested on the UI task and serviced on the decode loop,
 * and nothing measured the gap between those two. A press that takes
 * long enough to act reads as a press that was missed -- which is how
 * four presses of prev become four presses of prev, each one of them
 * making it worse.
 *
 * Logged with the request rather than inferred from timestamps, because
 * the interesting case is the request that is overwritten before it is
 * ever serviced: that one leaves no trace at all otherwise.
 */
static volatile TickType_t s_seek_asked;
static const char *volatile s_seek_why = "";

/*
 * True between entering and leaving play_file(), i.e. whenever there is
 * a decode loop to service a request.
 *
 * Needed because s_seek_pct is a request to a loop that may not exist.
 * At the end of a playlist nothing is playing, so a seek sits in the
 * variable until the next track starts -- and is then applied to that
 * track, which is not what was asked for and is fifteen seconds late.
 * That is exactly what happened on hardware: prev pressed after the last
 * track ended, then serviced when an unrelated folder was started.
 */
static volatile bool s_decoding;

static void request_seek(int pct, const char *why)
{
    /*
     * Refused rather than queued when there is nothing to seek in. A
     * request with no reader is not pending, it is lost -- and a lost
     * request that fires later, against a different song, is worse than
     * one that never fires at all.
     */
    if (!s_decoding) {
        ESP_LOGI(TAG, "seek (%s) ignored: nothing playing", why);
        return;
    }

    if (s_seek_pct >= 0) {
        /* The previous request never made it to the decoder. Said out
         * loud: from the outside this is indistinguishable from a button
         * that did nothing, and it is the reason repeated presses of the
         * same control appear to be ignored. */
        ESP_LOGW(TAG, "seek (%s) replaced an unserviced seek (%s) after %" PRIu32 " ms",
                 why, s_seek_why,
                 (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount() - s_seek_asked));
    }
    s_seek_why = why;
    s_seek_asked = xTaskGetTickCount();
    s_seek_pct = pct;
}

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
/*
 * Track identity, as a number.
 *
 * The frame walk is cancelled with a flag it polls; a JPEG decode is a
 * single call into a driver and cannot be. So media_task takes a copy of
 * this before it starts and checks it again before drawing: if the number
 * has moved, what it holds belongs to a song that is no longer playing
 * and goes in the bin.
 */
static volatile uint32_t s_track_gen;

/*
 * Whether s_pos_sec, s_len_sec and s_can_seek belong to the track whose
 * name is on screen.
 *
 * They used to be set just before the decode loop's first iteration,
 * which is after decoder_open() -- and decoder_open() on a Xing-less MP3
 * is a whole-file scan, twelve seconds on a USB drive. For all of it the
 * bar went on filling and the clock went on counting the *previous*
 * track, which is the most convincing possible way to look like the
 * press did nothing.
 *
 * Cleared by track_change_begin(), at the moment the decision is made,
 * for the same reason the generation counter is bumped there. Set again
 * only once the new numbers are real.
 */
static volatile bool     s_stats_valid;

/*
 * What the decoder says this file is, published for the format card.
 *
 * Only ever written by the decode loop on the first block of a track and
 * read by media_task, which is why it is a handful of scalars and one
 * short string rather than a struct behind a lock: media_task waits for
 * s_fmt_known and by then none of them are moving.
 */
static char              s_fmt_codec[16];
static volatile int      s_fmt_rate;
static volatile int      s_fmt_chans;
static volatile int      s_fmt_kbps;
static volatile bool     s_fmt_known;

static char              s_media_path[512];
static volatile bool     s_media_want;

/*
 * The track whose envelope is already computed.
 *
 * load_track_visuals() runs for two different reasons and they do not
 * want the same work. A new track needs everything. A repaint -- the
 * chooser closing, having drawn over the artwork -- needs the cover put
 * back and nothing else, because the envelope on screen is already this
 * track's.
 *
 * Without this, dismissing the chooser cost a full walk of the playing
 * file. Cancelling a folder with nothing playable in it read 30 MB off
 * the card for an envelope identical to the one already drawn:
 *
 *   /usb/FirmamentSoundtrack: 0 tracks
 *   nothing playable in /usb/FirmamentSoundtrack
 *   tags: "Doctor" / ...                     <- repaint of the playing track
 *   walk: 6043 frames, 157s, levels=1        <- and its envelope, again
 */
static char              s_walked_path[512];
static volatile bool     s_scan_abort;

/*
 * The prefetch walk's own abort flag.
 *
 * Separate from s_scan_abort deliberately. That one means "the walk of
 * the playing track is no longer wanted"; this one means "the track that
 * was going to be next is no longer next". They are set together on a
 * track change and cleared at different times -- media_task clears
 * s_scan_abort when it picks up the new track, and prefetch_next()
 * clears this one immediately before it starts scanning, which is the
 * only point at which "no longer next" has been re-decided.
 */
static volatile bool     s_prefetch_abort;
static volatile bool     s_wave_ready;
static framewalk_t       s_walk;            /* media_task's scan buffer */
static framewalk_t       s_walk_pending;    /* decode loop's; see below */

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

/*
 * What was actually played, newest last.
 *
 * playlist_prev() walks index-1, which is the right answer in list order
 * and the wrong one under shuffle -- playlist.h says so itself: a back
 * button that undoes a random choice needs a stack. This is that stack.
 *
 * Eight deep because it is a transport button, not a browser: pressing
 * back eight times is already past the point where anyone is retracing a
 * path rather than looking for something. Deeper costs nothing in memory
 * and everything in the odds that the entry is still cached.
 *
 * Paths are copied rather than pointed at, because playlist_load_dir()
 * invalidates every pointer the playlist handed out and the history has
 * to survive changing folders -- that is most of the point of it.
 */
#define HISTORY_DEPTH   (8)
static char s_history[HISTORY_DEPTH][512];
static int  s_history_n;

/*
 * Pushed when a track actually starts, not when one is requested.
 *
 * A requested track that never plays -- a file that vanished with its
 * volume, a skip landing on something unreadable -- must not become a
 * place the back button can return to.
 */
static void history_push(const char *path)
{
    if (!path || !*path) return;

    /* Not the same track twice in a row. Restarting a track, or the
     * decoder re-entering one, would otherwise fill the stack with a
     * single song and make back do nothing eight times. */
    if (s_history_n > 0 && strcmp(s_history[s_history_n - 1], path) == 0) return;

    if (s_history_n == HISTORY_DEPTH) {
        memmove(s_history[0], s_history[1], sizeof(s_history) - sizeof(s_history[0]));
        s_history_n--;
    }
    snprintf(s_history[s_history_n], sizeof(s_history[0]), "%s", path);
    s_history_n++;
}

/*
 * The track played before `anchor`, discarding anchor and anything after
 * it. NULL when anchor is not in the history or is the oldest entry.
 *
 * Anchored rather than simply popping the top, because of a race the
 * obvious version loses. The two taps of a double tap are up to 400 ms
 * apart and the first one already requested a track change -- so by the
 * time the second arrives, that track may or may not have started and
 * pushed itself. Popping the top therefore means "the track before the
 * one I was on" or "the track I was just on" depending on how fast the
 * card was, which is a back button that sometimes goes forward.
 *
 * The anchor is the track that was playing when the FIRST tap landed, so
 * the answer does not depend on what happened in between.
 */
static const char *history_back_from(const char *anchor)
{
    if (!anchor || !*anchor) return NULL;

    int i = s_history_n - 1;
    while (i >= 0 && strcmp(s_history[i], anchor) != 0) i--;
    if (i <= 0) return NULL;

    /* Truncate rather than just read: anchor and whatever the first tap
     * started are both ahead of where we are going, and leaving them
     * would make the next press walk forward through them. */
    s_history_n = i;
    return s_history[i - 1];
}

/* Where the current double-tap pair started. */
static char s_prev_anchor[512];
static const char *s_display_name = "";

/*
 * Everything a track change needs done immediately: the tags, a cleared
 * artwork area, and a request for the two slow jobs.
 *
 * It used to decode the cover here too, and that was the whole problem.
 * On the decode loop, which has 0.37 s of ring to spend, it was reading
 * half a megabyte off a USB drive and then handing it to a decoder that
 * takes 550 ms on a large picture. Both of those now belong to media_task;
 * what is left is tag parsing and one flag, which is microseconds.
 *
 * Still on the decode loop rather than the UI task: it fopen()s the track,
 * and the UI task has a 20 ms period to keep. */
/*
 * The moment the track changes, as distinct from the moment its details
 * are known.
 *
 * These were the same statement, at the top of load_track_visuals(), and
 * load_track_visuals() runs after decoder_open() -- which on a Xing-less
 * MP3 is a full scan of the file. Twelve seconds on a USB drive. For all
 * of that the screen kept the outgoing track's title, artist and cover,
 * and anything the background task had in flight for it still counted as
 * current:
 *
 *   playing /usb/aom/hotlantis.mp3          <- new track
 *   cover is 1920x1920                      <- previous track's cover,
 *   cover fitted to 720x720                    decoded and drawn anyway
 *   ...
 *   no ID3 text frames; showing the filename   <- 3.3 s later
 *
 * The generation check was not wrong, it was late: at the moment that
 * cover was drawn the new track had not yet reached the line that bumps
 * the counter, so the cover was, by the only definition available,
 * current. Invalidating when the decision is made rather than when its
 * consequences are computed is what makes the check mean what it says.
 *
 * Called from play_file() before decoder_open(), so the screen goes blank
 * and honest immediately instead of lying for as long as the open takes.
 */
/*
 * Title, artist and album, now rather than eventually.
 *
 * The cache first, because prefetch put them there a track ago and a hit
 * is a memcpy; otherwise a read, which is a couple of fread()s at the
 * front of the file and is measured in milliseconds even on USB. Either
 * way this is cheap enough to run on the decode loop before
 * decoder_open(), which is the whole point of it: the tag is available
 * long before the decoder has finished deciding what the file is.
 *
 * WHY THE FILENAME IS NOT SET FIRST
 *
 * It used to be. track_change_begin() pointed s_display_name at the
 * basename immediately and the tags landed later, so every track change
 * flashed "04 - track04.mp3" for as long as the tag read took and then
 * replaced it with the title. That reads as the player having failed to
 * find a title and then changing its mind -- and on a file that really
 * has no tag, the flash is indistinguishable from the final answer,
 * which is what made it look like a guess in both cases.
 *
 * So the filename is the fallback and is only reached as one. Between
 * the track change and this function returning, the title row is empty;
 * that gap is a few milliseconds and an empty row for it is honest,
 * where a filename for it is not.
 */
static void load_tags(const char *path)
{
    memset(&s_tags, 0, sizeof(s_tags));

    if (mediacache_tags(path, &s_tags)) {
        ESP_LOGI(TAG, "tags from cache: \"%s\"", s_tags.title);
    } else {
        FILE *af = storage_io_open(path, "rb");
        if (af) {
            if (covertag_read_tags(af, &s_tags) == ESP_OK) {
                ESP_LOGI(TAG, "tags: \"%s\" / \"%s\" / \"%s\"",
                         s_tags.title, s_tags.artist, s_tags.album);
                /* Read here, stored by media_task. This runs on the
                 * decode loop, and storing is what evicts -- which is
                 * media_task's alone, because media_task is the one
                 * holding borrowed pointers into these entries. See the
                 * threading note in mediacache.h. */
            } else {
                memset(&s_tags, 0, sizeof(s_tags));
                ESP_LOGI(TAG, "no text tags in this file; showing the filename");
            }
            storage_io_close(af);
        }
    }

    /* Only now, and only if there is nothing better. */
    if (!s_tags.title[0]) {
        s_display_name = strrchr(path, '/');
        s_display_name = s_display_name ? s_display_name + 1 : path;
    } else {
        s_display_name = "";
    }
}

/*
 * True from the moment a track change starts until the decode loop for
 * it has finished, one way or another.
 *
 * The amplifier's idle hold is 1500 ms and its comment says why: the gap
 * between two tracks "is a few hundred milliseconds at most", so waiting
 * out a fixed 1.5 s stops a boundary clicking the output stage off and
 * on. That was true when it was written. It stopped being true when the
 * index build made the gap fifteen seconds -- invisible then, because
 * the silence was so long that a click at either end of it was the least
 * of the problem -- and it is false again now for a different reason:
 * 0104 brought the gap to about 1.8 s, which is just over the hold, so
 * every track change clicks off and back on 19 ms later.
 *
 * A fixed timeout cannot be right for a gap whose length depends on the
 * size of the next file. So idle stops being inferred from "nothing is
 * being decoded" and starts meaning what it says: nothing is going to be
 * played soon. During a track change something certainly is.
 */
static volatile bool s_track_changing;

static void track_change_begin(const char *path)
{
    s_track_gen++;

    s_scan_abort = true;
    s_prefetch_abort = true;
    s_wave_ready = false;

    /*
     * Re-pin around the change. The track leaving the screen is the one
     * a back button is for, so it keeps a slot; everything else becomes
     * evictable, including whatever was pinned two tracks ago.
     *
     * unpin-then-pin rather than an explicit unpin of the old entry,
     * because the set of things worth keeping is defined by where
     * playback is now, not by tracking each transition. With three
     * slots, two pins, that leaves exactly one for prefetch -- which is
     * the intended shape.
     */
    mediacache_unpin_all();
    if (s_path[0]) mediacache_pin(s_path);      /* the outgoing track */

    /*
     * Everything the old track's numbers said, retired here rather than
     * when the new ones arrive. See s_stats_valid: the gap between the
     * two is a whole-file scan on some formats, and a bar that keeps
     * filling across it is the previous song's progress drawn under this
     * song's name.
     */
    s_stats_valid = false;
    s_pos_sec = 0;
    s_len_sec = 0;
    s_can_seek = false;
    s_fmt_known = false;
    s_fmt_rate = s_fmt_chans = s_fmt_kbps = 0;
    s_fmt_codec[0] = '\0';

    /*
     * The envelope, from the cache when prefetch got there first.
     *
     * Three cases and they are all here: the walk we already have for
     * this exact path (a repaint, or a track that never changed), the
     * one prefetch left in the cache -- which arrives fully drawn, with
     * no scan and no wait -- and nothing, which blanks it.
     */
    if (strcmp(path, s_walked_path) != 0) {
        /*
         * Its own buffer, not s_walk.
         *
         * s_walk is where framewalk_scan() writes, and that scan belongs
         * to media_task -- which may still be part-way through the
         * previous track's when this runs. Copying a cached envelope
         * into it would be two tasks writing one kilobyte at once, for
         * no reason: waveform_set() takes a copy anyway, so this buffer
         * is dead the moment the call returns.
         *
         * And the copy accessor, not the borrowing one: this is the
         * decode loop, and a pointer into a cache entry is only safe in
         * the hands of the task that evicts.
         */
        if (mediacache_walk_copy(path, &s_walk_pending)) {
            waveform_set(&s_walk_pending);
            snprintf(s_walked_path, sizeof(s_walked_path), "%s", path);
            ESP_LOGI(TAG, "envelope from cache at track change");
        } else {
            waveform_set(NULL);
        }
    }

    /* Read before the screen is asked to show anything, so the title row
     * is either right or empty and never a filename standing in for a
     * title the file actually has. */
    load_tags(path);

    ui_clear_art();
}

static void load_track_visuals(const char *path)
{
    /*
     * No invalidation here any more -- track_change_begin() did it, at
     * the moment the track changed. What is left is the part that needs
     * the file open.
     *
     * This is also the repaint path, which is the other reason the two
     * had to come apart: the chooser closing wants the cover put back and
     * nothing thrown away.
     */
    /* The tags are already in hand -- track_change_begin() read them
     * before decoder_open(), and the cache makes this path free on a
     * repaint, where they have not changed at all. */
    load_tags(path);

    /* Cleared here as well as in track_change_begin(), because this is
     * also the repaint path: the chooser has just been drawing over the
     * artwork area and whatever it left there has to go before the cover
     * is painted back. On a track change it is a second clear of an
     * already black area, which is a memset nobody can see. */
    ui_clear_art();

    /* Both of the slow jobs for this track, requested rather than done,
     * and both on one task. Anything in flight for the outgoing track was
     * invalidated by track_change_begin(). */
    snprintf(s_media_path, sizeof(s_media_path), "%s", path);
    s_media_want = true;
}

/*
 * Reads the cover out of the tag, decodes it, and puts it on screen.
 *
 * Its own task, because it is slow and the decode loop cannot afford to
 * be. The ring is 64 KB -- 0.37 s of 44.1 kHz stereo -- and the hardware
 * JPEG decode of a 3000x3000 cover took 550 ms on its own, before the
 * 511 KB read off a USB drive that precedes it. Everything this function
 * does used to happen inline in load_track_visuals(), on the decode loop,
 * which means every large cover was spending longer than the ring holds.
 *
 * What makes a second drawing task allowable is that gfx.c now serialises
 * blits properly -- a mutex, and a wait on the panel's completion
 * callback -- so "one writer to the framebuffer" is enforced rather than
 * arranged. Before that it was true only because the tasks that drew took
 * turns by construction.
 *
 * The two writers still own disjoint rows: this task paints the artwork
 * area and ui_task paints the bar below it.
 */
/* Extension, upper case, without the dot. "FILE" when there is not one
 * -- which is rarer than it sounds, since the browser only lists things
 * it recognised by extension in the first place. */
static void container_name(const char *path, char *out, size_t out_len)
{
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    if (!dot || (slash && dot < slash) || !dot[1]) {
        snprintf(out, out_len, "FILE");
        return;
    }
    size_t i = 0;
    for (const char *p = dot + 1; *p && i + 1 < out_len; p++, i++) {
        out[i] = (*p >= 'a' && *p <= 'z') ? (char)(*p - 32) : *p;
    }
    out[i] = '\0';
}

/*
 * What goes where the cover would have gone.
 *
 * A file with no picture used to get 720x720 of black, which is exactly
 * what a cover that has not arrived yet looks like -- so the two states
 * the player most needs to distinguish were drawn identically, and the
 * honest one was the one that looked broken. The format is the thing
 * every file can say about itself, and it is worth reading: a track that
 * turns out to be 96 kbps mono explains itself at a glance.
 *
 * The decoder is the source for everything but the container and the
 * size, so this waits briefly for the first block to be decoded. Not
 * long, and not forever: by the time media_task runs, the ring has been
 * filling for the best part of a second and s_fmt_known is set. The
 * bound is for the case where it is not -- a paused start, a stalled
 * device -- where the card appears with two lines instead of four rather
 * than not appearing.
 */
#define FMT_WAIT_SLICE_MS   (50)
#define FMT_WAIT_MAX_MS     (600)

static void show_format_card(const char *path, long bytes, uint32_t gen)
{
    for (int waited = 0; !s_fmt_known && waited < FMT_WAIT_MAX_MS;
         waited += FMT_WAIT_SLICE_MS) {
        vTaskDelay(pdMS_TO_TICKS(FMT_WAIT_SLICE_MS));
        if (gen != s_track_gen) return;
    }
    if (gen != s_track_gen) return;

    char head[16];
    char rate[48] = "";
    char codec[48] = "";
    char size[32] = "";

    container_name(path, head, sizeof(head));

    if (s_fmt_known && s_fmt_rate > 0) {
        snprintf(rate, sizeof(rate), "%d Hz  %s", s_fmt_rate,
                 s_fmt_chans == 1 ? "mono"
                 : s_fmt_chans == 2 ? "stereo" : "multichannel");
    }
    if (s_fmt_known && s_fmt_kbps > 0) {
        snprintf(codec, sizeof(codec), "%s  %d kbps",
                 s_fmt_codec[0] ? s_fmt_codec : head, s_fmt_kbps);
    } else if (s_fmt_known && s_fmt_codec[0]) {
        snprintf(codec, sizeof(codec), "%s", s_fmt_codec);
    }
    if (bytes > 0) {
        /* One decimal place, integer arithmetic -- MB is the only unit
         * worth having here and a cover-less file is never small enough
         * for KB to read better. */
        const long tenths = (bytes * 10) / (1024 * 1024);
        snprintf(size, sizeof(size), "%ld.%ld MB", tenths / 10, tenths % 10);
    }

    const char *lines[5];
    int n = 0;
    lines[n++] = head;
    if (rate[0])  lines[n++] = rate;
    if (codec[0]) lines[n++] = codec;
    if (size[0])  lines[n++] = size;
    lines[n++] = "no cover art";

    ui_show_art_info(lines, n);
}

static void do_art(const char *path, uint32_t gen)
{
    /*
     * Already known to have no picture in it -- prefetch read the tag, or
     * this track was played earlier. Straight to the card, with no read
     * at all. Without the negative being cached this is a full tag scan
     * every time the track comes back, to learn the same nothing.
     */
    if (mediacache_no_art(path)) {
        ESP_LOGI(TAG, "no cover art (cached); showing the format");
        long known = 0;
        FILE *sf = fopen(path, "rb");        /* size only: open, seek, close */
        if (sf) {
            if (fseek(sf, 0, SEEK_END) == 0) known = ftell(sf);
            fclose(sf);
        }
        show_format_card(path, known, gen);
        return;
    }

    FILE *af = storage_io_open(path, "rb");
    if (!af) return;

    long fsize = 0;
    if (fseek(af, 0, SEEK_END) == 0) fsize = ftell(af);
    rewind(af);

    uint8_t *jpg = NULL;
    size_t jpg_len = 0;
    esp_err_t xerr;

    /*
     * The cache first, which is what makes a prefetched track and a
     * track being returned to both appear immediately. `owned` says
     * whether the bytes are ours to free: a cache hit is borrowed, and
     * freeing it would leave the cache pointing at a freed block.
     */
    bool owned = false;
    size_t cached_len = 0;
    const uint8_t *cached = mediacache_art(path, &cached_len);

    if (cached) {
        jpg = (uint8_t *)cached;
        jpg_len = cached_len;
        xerr = ESP_OK;
        storage_io_close(af);
        ESP_LOGI(TAG, "cover from cache (%u bytes)", (unsigned)jpg_len);
    } else {
        xerr = covertag_extract_art(af, &jpg, &jpg_len);
        storage_io_close(af);
        owned = true;
    }

    if (xerr != ESP_OK) {
        /* Now genuinely "no picture in the file" for MP3, FLAC, M4A,
         * Ogg and WAV alike, rather than "no APIC frame" -- which was
         * what it used to mean, and was why every FLAC on the card
         * showed a blank square. ESP_ERR_NOT_SUPPORTED still means the
         * container has no parser here at all. */
        ESP_LOGI(TAG, "no cover art in this file (%s)", esp_err_to_name(xerr));

        /* Remembered, so a return to this track does not read the tag
         * again to find the same nothing, and so the card can be put up
         * without a read at all.
         *
         * Only for the two errors that are statements about the file. A
         * failed allocation or a short read says nothing about whether
         * there is a picture in there, and caching it as "none" would
         * make one bad moment permanent for as long as the entry
         * survives. */
        if (xerr == ESP_ERR_NOT_FOUND || xerr == ESP_ERR_NOT_SUPPORTED) {
            mediacache_put_no_art(path);
        }

        if (gen == s_track_gen) show_format_card(path, fsize, gen);
        return;
    }

    /*
     * The track may have moved on during the read. Decoding anyway
     * would put the previous song's cover over the current song's
     * screen, and unlike the envelope -- which is only ever drawn
     * once, from a flag -- there is no later redraw to correct it.
     */
    if (gen != s_track_gen) {
        ESP_LOGI(TAG, "cover arrived after the track changed; dropped");
        /* Into the cache rather than the bin. It was read for a track
         * that moved on, but the track it belongs to is very likely the
         * one being returned to -- a fast double-skip lands here, and
         * throwing the bytes away means reading them again. */
        if (owned) mediacache_put_art(path, jpg, jpg_len);
        return;
    }

    /* A square, and the full width of the panel. albumart.c fits the
     * decoded cover into whatever rectangle it is handed; handing it
     * a square is what stops it letterboxing a square cover into a
     * tall box with black above and below. */
    const esp_err_t serr = albumart_show(s_panel, LCD_H_RES, UI_ART_H,
                                         jpg, jpg_len);

    /* Kept, not freed, when we own it: this is the playing track, so it
     * is about to be pinned and is the thing a back button wants. The
     * cache takes ownership; a borrowed hit is left alone. */
    if (owned) mediacache_put_art(path, jpg, jpg_len);

    if (serr != ESP_OK) {
        ESP_LOGW(TAG, "cover art failed to decode (%s)",
                 esp_err_to_name(serr));
        return;
    }

    /* One more generation check. The decode is the long part, and a
     * cover blitted for a track that has already been replaced is
     * exactly what the first check was avoiding. */
    if (gen != s_track_gen) {
        ESP_LOGI(TAG, "cover decoded after the track changed; not shown");
        ui_clear_art();
    }

}

/*
 * Walks the current track and stops when asked.
 *
 * Its own FILE*, deliberately: the decoder holds one and this seeks to
 * EOF and back. Sharing it would be a seek war with the thing producing
 * the audio.
 */
static void do_walk(const char *path)
{
    /* Silent: the reason is logged once at boot rather than once per
     * track, since it does not change while the firmware is running. */
    if (!WAVEFORM_SCAN) return;

    /*
     * A cached envelope, which is the entire point of caching it: the
     * walk is a whole-file read, so returning to a track otherwise costs
     * the same 30 MB it cost the first time. A kilobyte in PSRAM buys
     * that back.
     */
    const framewalk_t *hit = mediacache_walk(path);
    if (hit) {
        memcpy(&s_walk, hit, sizeof(s_walk));
        s_wave_ready = true;
        ESP_LOGI(TAG, "envelope from cache: %d columns", s_walk.columns);
        return;
    }

    /*
     * Second-cheapest lookup, before the whole-file walk: the on-disk
     * sidecar. mediacache_walk() above only ever has an answer if this
     * track was already visited since boot; the sidecar survives a
     * power cycle. A hit here still goes through mediacache_put_walk()
     * below, on the framewalk_t constructed from it, so a second look
     * at the same track in this session is the RAM hit above.
     */
    replaygain_t rg;
    if (replaygain_load(path, &rg) && rg.has_levels) {
        memset(&s_walk, 0, sizeof(s_walk));
        s_walk.has_levels = true;
        s_walk.columns = REPLAYGAIN_COLUMNS;
        s_walk.frames = rg.frames;
        s_walk.sec = rg.sec;
        memcpy(s_walk.level, rg.waveform, sizeof(rg.waveform));
        s_wave_ready = true;
        mediacache_put_walk(path, &s_walk);
        ESP_LOGI(TAG, "envelope from sidecar: %d columns, %" PRIu32 "s",
                 s_walk.columns, s_walk.sec);
        return;
    }
    /* rg.has_levels == false (AAC/AMR) falls through to the walk below
     * rather than being treated as a hit: those formats have nothing to
     * show either way, and the walk still supplies the duration that
     * the sidecar does not carry (see replaygain.h). */

    FILE *f = storage_io_open(path, "rb");
    if (!f) return;

    /* Decline before reading anything. Walking a format with no
     * parser here is a whole file off the card in exchange for a log
     * line saying it found nothing -- and it is contention with the
     * decoder reading the same card for the same track. */
    if (!framewalk_supports(f)) {
        ESP_LOGI(TAG, "no frame walker for this format; no envelope");
        storage_io_close(f);
        return;
    }

    /* One column per pixel of panel width, still, even though the bar
     * is narrower than that now. The extra columns cost a kilobyte
     * and are averaged down at draw time; the alternative is a rescan
     * whenever the bar's width changes. */
    const int cols = LCD_H_RES;

    const esp_err_t err = framewalk_scan(f, cols, &s_scan_abort, &s_walk);
    storage_io_close(f);

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
        ESP_LOGI(TAG, "envelope ready: %d columns", s_walk.columns);
        s_wave_ready = true;
        mediacache_put_walk(path, &s_walk);
        /* Best-effort. A failed write costs the next play a re-walk --
         * the state every play was in before this file existed -- so
         * the error is logged inside replaygain_save() and not acted
         * on here. */
        replaygain_save(path, &s_walk);
    }
}

/*
 * How full the PCM ring is, 0-100, or -1 when nothing is playing.
 *
 * This is the whole throttle. Prefetch is a second reader on the device
 * the decoder is already reading, and on a USB drive that contention is
 * exactly what the one-task-for-both design was built to avoid -- so
 * rather than guess a safe moment, ask the thing that would suffer.
 *
 * A percentage rather than a byte count because the ring size is a
 * tuning knob and the thresholds should not have to move with it.
 */
static int ring_headroom_pct(void)
{
    return s_ring_pct;
}

/*
 * Start prefetching only above HIGH, and only while it stays above LOW.
 *
 * Two thresholds rather than one because a single one chatters: at
 * exactly the boundary the prefetch would start, take a read, drop
 * below, abort, refill, start again, and spend the whole track doing
 * that. The gap is hysteresis.
 *
 * HIGH is deliberately near the top. Prefetch is never urgent -- the
 * worst case is the cover arriving when the track starts, which is what
 * happens today -- so it should only ever run out of genuine surplus.
 */
#define PREFETCH_START_PCT      (75)
#define PREFETCH_ABORT_PCT      (50)

/* Consecutive unreadable tracks before the player stops walking the
 * folder. See the note at the use site. */
#define UNREADABLE_LIMIT        (3)

/*
 * Republish the gauge.
 *
 * Called from the decode loop, which is where it used to live, AND from
 * the writer task -- because the decode loop is exactly the task that
 * stops running when the gauge matters most.
 *
 * PREFETCH_ABORT_PCT is checked against this number by a walk running on
 * media_task, and that walk holds the card's VFS lock across 32 KB
 * reads. The decode loop's next read blocks behind it, so it stops
 * decoding, so it stops publishing -- and the gauge freezes at whatever
 * it read last, which by construction is above PREFETCH_START_PCT
 * because that is the gate the walk had to pass to start. The abort
 * therefore never fires. The ring drains to nothing, the audio stops for
 * as long as the scan takes, and the one number that could have stopped
 * it is sitting there reporting the ring as healthy.
 *
 * The writer task is the right second publisher: it never touches the
 * card, so nothing on the card can stop it, and it wakes on a 100 ms
 * timeout even with an empty ring. A number whose freshness depends on
 * the thing it is meant to be watching is not a gauge.
 */
/*
 * Seconds heard, from frames decoded minus frames still queued.
 *
 * Decoded is not played: the ring sits between the decoder and the
 * speaker, and every sample in it has been counted by the decode loop
 * and not yet heard. That was always true and never mattered -- at 64 KB
 * the ring held 0.37 s, under the resolution of the display -- but at
 * 10 MB it is a minute, and the position ran a minute ahead of the audio.
 *
 * Called from the writer task, which is the one task that runs for the
 * whole life of the ring. The decode loop exits when the file ends and
 * the writer keeps going until the ring is empty, so computing this in
 * the decode loop froze the display at (length - 59 s) and left the
 * remaining time stuck at -00:59 for the last minute of every track.
 *
 * Two bytes per sample per channel, so bytes / (2 * channels) is the
 * queued frame count. Subtracted in frames rather than seconds so a
 * large number is not rounded twice, and clamped because the ring can
 * report more than has been decoded right after a seek reset.
 */
static void pos_publish(size_t queued)
{
    const uint32_t rate = s_frames_rate;
    if (!rate) return;

    const uint32_t chans = s_frames_chans ? s_frames_chans : 1;
    const uint64_t queued_frames = queued / (2 * chans);
    const uint64_t decoded = s_frames_out;
    const uint64_t heard = decoded > queued_frames ? decoded - queued_frames : 0;
    s_pos_sec = (uint32_t)(heard / rate);
}

static void ring_publish(void)
{
    const size_t queued = xStreamBufferBytesAvailable(s_pcm);
    s_ring_pct = (int)((queued * 100) / PCM_RING_BYTES);
    pos_publish(queued);

    /* And act on it. Nothing else was: s_prefetch_abort was set on a
     * track change and nowhere else, so the low-water threshold the
     * prefetch documents was only ever consulted between stages, never
     * during the one stage long enough to need it. */
    if (s_ring_pct >= 0 && s_ring_pct < PREFETCH_ABORT_PCT) {
        s_prefetch_abort = true;
    }
}

/*
 * How long media_task waits after a track change before touching the
 * card, and how full the ring has to be before it starts.
 *
 * media_task is at priority 1, against 4 for the UI and 6 for the I2S
 * writer, but priority only decides who gets the CPU -- it does nothing
 * about who gets the device.
 * A background task at priority 1 issuing a 512 KB read still puts that
 * read in the same queue as the decoder's, and the decoder then waits
 * behind it no matter how important it is.
 *
 * So the throttle is time and depth, not priority. The delay lets the
 * ring fill from empty after a track change -- which is exactly when the
 * decoder needs the device most and when the old code piled a cover read
 * and a whole-file walk on top of it. ART_DELAY is short because the
 * cover is the visible thing; WALK_DELAY is long because the envelope is
 * not, and because the walk reads the entire file.
 */
#define MEDIA_ART_DELAY_MS      (700)
/* Short, because the cover stage above has already settled and the ring
 * has been filling throughout it. The wait that matters here is the one
 * on the ring, not this. */
#define MEDIA_PREFETCH_DELAY_MS (250)
#define MEDIA_WALK_DELAY_MS     (2500)
#define MEDIA_MIN_RING_PCT      (60)
#define MEDIA_WAIT_SLICE_MS     (100)
#define MEDIA_WAIT_MAX_MS       (8000)

/*
 * Sleep for `delay_ms`, then wait for the ring to reach `floor_pct`.
 *
 * The floor is a parameter because prefetch wants a higher one than the
 * cover does, and having two numbers in two places was the whole of the
 * 0101 prefetch regression: this waited for 60% while prefetch_next()
 * sampled once and demanded 75%, so the answer depended on how long
 * whatever ran before it happened to take.
 *
 * Returns false if the track changed while waiting, which means whatever
 * was about to be done is for a song nobody is listening to any more.
 *
 * The wait is bounded: a format the ring never fills for -- a slow
 * device, a very high bitrate -- must not mean the cover never loads at
 * all. After MEDIA_WAIT_MAX_MS it proceeds anyway and says so, because a
 * late cover beats no cover, and the log line is how that gets noticed
 * rather than being silently lived with.
 */
static bool media_settle(uint32_t gen, int delay_ms, int floor_pct,
                         const char *what)
{
    int waited = 0;

    while (waited < delay_ms) {
        vTaskDelay(pdMS_TO_TICKS(MEDIA_WAIT_SLICE_MS));
        waited += MEDIA_WAIT_SLICE_MS;
        if (gen != s_track_gen) return false;
    }

    while (waited < MEDIA_WAIT_MAX_MS) {
        const int pct = ring_headroom_pct();
        if (pct < 0 || pct >= floor_pct) return true;
        vTaskDelay(pdMS_TO_TICKS(MEDIA_WAIT_SLICE_MS));
        waited += MEDIA_WAIT_SLICE_MS;
        if (gen != s_track_gen) return false;
    }

    ESP_LOGW(TAG, "%s: ring never reached %d%% in %d ms; going ahead anyway",
             what, floor_pct, MEDIA_WAIT_MAX_MS);
    return true;
}

/*
 * Get the next track ready: tags, cover, envelope. In that order.
 *
 * Everything a track change makes the user wait for is fetched here, a
 * track early, into the three-entry cache -- so arriving at the next
 * song is a memcpy rather than three reads off a device the decoder is
 * already using. That is the entire reason a "load" felt slow: none of
 * this work could start until the track it was for was already playing.
 *
 * The order is the order they are wanted in. Tags are first because they
 * are tiny and because the title is the first thing to appear; the cover
 * is second because it is the largest thing on screen and its absence is
 * what reads as a stall; the envelope is last because it is a whole-file
 * read and its absence is invisible -- the bar degrades to a plain
 * slider and fills in later, which is what it already did.
 *
 * Each stage re-checks the gate. They are not one operation: the tags
 * cost a few KB and the walk costs the whole file, and a device that can
 * spare the first cannot necessarily spare the last.
 *
 * The walk is abortable through s_prefetch_abort, threaded into
 * framewalk_scan()'s existing polling. It has to be -- a gate checked
 * once at the start is fine for a 120 KB bounded read and is not fine
 * for a 60 MB one that would otherwise run for seconds after the track
 * it was for stopped being next.
 */
static framewalk_t s_prefetch_walk;     /* media_task only; ~1 KB, not a local */

/* Whether the ring can still spare a reader. Logged by the caller when
 * it cannot, because a gate that never opens is indistinguishable from a
 * feature that was never built. */
static bool prefetch_ok(int floor_pct)
{
    const int pct = ring_headroom_pct();
    return pct < 0 || pct >= floor_pct;
}

static void prefetch_next(void)
{
    const char *next = playlist_peek_next(browser_order());
    if (!next) {
        ESP_LOGD(TAG, "prefetch: nothing next (order/end of folder)");
        return;
    }

    /*
     * No start gate here any more: media_task waits on
     * PREFETCH_START_PCT through media_settle() before calling this, so
     * re-testing it would only re-introduce the sample-once failure one
     * level down -- and would throw away the work when the settle had
     * deliberately proceeded on its bounded timeout.
     *
     * The abort checks below stay. They are a different question: not
     * "is there surplus to start" but "has the surplus gone while I was
     * working", and that one still has to be asked repeatedly.
     */
    const uint32_t gen = s_track_gen;

    /* ---- tags ---- */
    if (!mediacache_tags(next, NULL)) {
        FILE *f = storage_io_open(next, "rb");
        if (f) {
            id3_tags_t t;
            memset(&t, 0, sizeof(t));
            if (covertag_read_tags(f, &t) == ESP_OK) {
                mediacache_put_tags(next, &t);
                ESP_LOGI(TAG, "prefetched tags: \"%s\"", t.title);
            }
            storage_io_close(f);
        }
    }

    if (gen != s_track_gen) return;

    /* ---- cover ---- */
    size_t have = 0;
    if (!mediacache_art(next, &have) && !mediacache_no_art(next)) {
        if (!prefetch_ok(PREFETCH_ABORT_PCT)) {
            ESP_LOGI(TAG, "prefetch stopped before the cover (ring %d%%)",
                     ring_headroom_pct());
            return;
        }

        FILE *f = storage_io_open(next, "rb");
        if (!f) return;

        uint8_t *img = NULL;
        size_t len = 0;
        const esp_err_t err = covertag_extract_art(f, &img, &len);
        storage_io_close(f);

        if (err != ESP_OK) {
            /* The useful half of what was learned. Without storing it,
             * arriving at this track re-reads the tag to find the same
             * nothing before it can put the format card up. Same two
             * errors as do_art(), for the same reason. */
            if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_NOT_SUPPORTED) {
                mediacache_put_no_art(next);
            }
        } else {
            /* Checked again on the way out. The read is bounded but not
             * instant, and a cover that cost the decoder its margin is
             * worse than no cover -- so if the ring fell through the
             * floor while this was reading, the result is dropped and
             * the log line says the gate is set too loose for this
             * device. */
            if (!prefetch_ok(PREFETCH_ABORT_PCT)) {
                ESP_LOGI(TAG, "prefetch cost too much ring (%d%%); dropped",
                         ring_headroom_pct());
                free(img);
                return;
            }
            mediacache_put_art(next, img, len);     /* takes ownership */
            ESP_LOGI(TAG, "prefetched cover for %s (%u bytes)",
                     next, (unsigned)len);
        }
    }

    if (gen != s_track_gen) return;

    /* ---- envelope ---- */
    if (WAVEFORM_SCAN && !mediacache_walk(next)) {
        if (!prefetch_ok(PREFETCH_START_PCT)) {
            ESP_LOGI(TAG, "prefetch stopped before the envelope (ring %d%%)",
                     ring_headroom_pct());
            return;
        }

        FILE *f = storage_io_open(next, "rb");
        if (!f) return;

        if (!framewalk_supports(f)) {
            storage_io_close(f);
        } else {
            /* Cleared here and nowhere else: this is the moment "still
             * next" was last true. */
            s_prefetch_abort = false;

            const esp_err_t err = framewalk_scan(f, LCD_H_RES,
                                                 &s_prefetch_abort,
                                                 &s_prefetch_walk);
            storage_io_close(f);

            if (err == ESP_OK && !s_prefetch_abort &&
                s_prefetch_walk.has_levels && s_prefetch_walk.frames &&
                gen == s_track_gen) {
                mediacache_put_walk(next, &s_prefetch_walk);
                /* Same sidecar a foreground walk writes in do_walk() --
                 * without this, a track that is only ever reached by
                 * prefetch (skipped past before its own do_walk() runs)
                 * never gets one, and the next time anything actually
                 * plays it, it pays for a full scan again. Best-effort,
                 * same as there. */
                replaygain_save(next, &s_prefetch_walk);
                ESP_LOGI(TAG, "prefetched envelope for %s (%d columns)",
                         next, s_prefetch_walk.columns);
            } else {
                ESP_LOGI(TAG, "prefetch walk abandoned");
            }
        }
    }

    int n = 0;
    size_t bytes = 0;
    mediacache_stats(&n, &bytes);
    ESP_LOGI(TAG, "prefetch done: cache %d entries, %u KB", n,
             (unsigned)(bytes / 1024));
}

/*
 * One background task for both slow per-track jobs, in that order.
 *
 * They were two tasks, and on a USB drive that was the wrong shape. Both
 * open the same file on the same slow device at the same moment -- the
 * cover reads half a megabyte out of the tag, the walk reads the whole
 * file -- so they spent the first seconds of every track taking turns at
 * the same queue and finishing later than either would have alone. One
 * task at a time is not slower here; it is the same total read with the
 * contention removed.
 *
 * Art first, and not because it is smaller. The cover is the largest
 * thing on screen and a track change blanks it, so the seconds before it
 * arrives are the ones that read as the player having stalled. The
 * envelope arriving late is invisible -- the bar falls back to a plain
 * slider and then becomes a waveform, which is what it already did.
 *
 * One stack rather than two, and it is the larger of the two, because the
 * cover path is the deeper one.
 */
static void media_task(void *arg)
{
    (void)arg;
    char path[512];

    while (1) {
        if (!s_media_want) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        s_media_want = false;
        s_scan_abort = false;
        const uint32_t gen = s_track_gen;
        snprintf(path, sizeof(path), "%s", s_media_path);

        /*
         * Out of the way of the track starting. See media_settle().
         *
         * Skipped entirely when the answer is already in memory, which
         * after a prefetch it usually is. The delay exists to keep a
         * second reader off the device while the ring fills; a cache hit
         * is not a reader, and making it wait 700 ms for the sake of
         * symmetry would throw away most of what the prefetch bought.
         * "Already cached" includes knowing there is no cover: that puts
         * the format card up, and it is not a read either.
         */
        size_t cached_len = 0;
        const bool art_in_hand = mediacache_art(path, &cached_len) != NULL ||
                                 mediacache_no_art(path);

        if (!art_in_hand && !media_settle(gen, MEDIA_ART_DELAY_MS, MEDIA_MIN_RING_PCT, "cover")) {
            continue;
        }

        HEAP_CHECK("before do_art");
        do_art(path, gen);
        HEAP_CHECK("after do_art");

        /*
         * The tags the decode loop read a moment ago, stored -- by the
         * task that is allowed to store them. It cost a read either way;
         * this is what makes the back button's return to this track free.
         * After the cover, because the cover is the thing on screen and
         * this is a few KB at the front of a file.
         */
        if (gen == s_track_gen && !mediacache_tags(path, NULL)) {
            FILE *tf = storage_io_open(path, "rb");
            if (tf) {
                id3_tags_t t;
                memset(&t, 0, sizeof(t));
                if (covertag_read_tags(tf, &t) == ESP_OK) {
                    mediacache_put_tags(path, &t);
                }
                storage_io_close(tf);
            }
        }

        /* A track change during the cover makes the walk pointless too --
         * s_media_want is already set for the new one, and starting this
         * walk would only mean aborting it a moment later. */
        if (gen != s_track_gen) continue;

        /*
         * The envelope, unless it is already drawn.
         *
         * Two ways it can be: this is a repaint of the track that was
         * already walked, or track_change_begin() installed a prefetched
         * walk out of the cache. Either way the shape on screen is this
         * track's and walking again would read the whole file to produce
         * the same numbers.
         *
         * Skipped rather than `continue`d, which it used to be -- the
         * prefetch below is the next track's business and has nothing to
         * do with whether this one needed a walk. Continuing here meant
         * that the better the cache did, the less prefetching happened,
         * which is precisely backwards.
         */
        /* WAVEFORM_SCAN gates the settle as well as the walk. Waiting
         * 2.5 s to then do nothing is the kind of thing that survives a
         * feature being switched off and is only found later, as a
         * prefetch that starts inexplicably late. */
        if (WAVEFORM_SCAN && strcmp(path, s_walked_path) != 0) {
            /* The expensive one: a whole-file read, so it waits longest
             * and is the first thing dropped when the track moves on. A
             * cached envelope skips the wait, because do_walk() will not
             * touch the card at all. */
            if (!mediacache_walk(path) &&
                !media_settle(gen, MEDIA_WALK_DELAY_MS, MEDIA_MIN_RING_PCT,
                              "envelope")) continue;

            do_walk(path);
            HEAP_CHECK("after do_walk");
            if (s_wave_ready) snprintf(s_walked_path, sizeof(s_walked_path),
                                       "%s", path);
        }

        /*
         * Everything for the playing track is in hand. Pin it so the
         * prefetch below cannot evict what is on screen, then use
         * whatever surplus the ring has to get a head start on the next
         * one.
         *
         * Ordered this way on purpose: prefetch runs after the current
         * track's own cover and envelope, never before. The next track's
         * artwork is worth nothing compared to this one's, and racing
         * them for the same device would make the visible one late to
         * make an invisible one early.
         */
        if (gen == s_track_gen) {
            mediacache_pin(path);

            /*
             * Wait for the surplus rather than sampling for it.
             *
             * 0101 switched the walk off and this stopped happening at
             * all -- "prefetch held off: ring at 61%, need 75%", every
             * track, forever. Nothing about prefetch had changed. What
             * changed is that the walk's 2.5 s settle used to run first,
             * and by the time it returned the ring was past 75%; without
             * it prefetch_next() sampled a ring that was still filling
             * and gave up, and nothing called it again.
             *
             * A gate that opens or not depending on how long an
             * unrelated stage happened to take is not a gate. This waits
             * on the number it actually cares about, with the same
             * bounded timeout as everything else -- a late prefetch
             * beats no prefetch, and PREFETCH_ABORT_PCT still stops the
             * work if the ring falls away underneath it.
             */
            if (media_settle(gen, MEDIA_PREFETCH_DELAY_MS,
                             PREFETCH_START_PCT, "prefetch")) {
                prefetch_next();
                HEAP_CHECK("after prefetch");
            }
        }
    }
}

/* Hand a chosen path to the decode loop. */
static void request_track(const char *path)
{
    /*
     * Any outstanding seek was aimed at the track being left. Carrying
     * it into the next one means starting a song at a position chosen
     * for a different song -- or, at the end of a playlist, resurrecting
     * a press from a minute ago.
     */
    if (s_seek_pct >= 0) {
        ESP_LOGI(TAG, "dropping unserviced seek (%s): track changed", s_seek_why);
        s_seek_pct = -1;
    }

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
            touch_swallow();
            browser_open(s_path);

            /*
             * Draw the chooser and start the next iteration, rather than
             * falling through to browser_touch() below.
             *
             * touch_swallow() gates touch_get(), and `bdown` was read at
             * the top of this iteration -- before the swallow existed.
             * Falling through hands the chooser that already-taken
             * sample, which is the press that opened it, at the folder
             * icon's coordinates, and browser_open() has just reset the
             * chooser's edge detector so it reads as a first tap.
             *
             * That is the whole bug, and it is why swallowing alone did
             * not fix it: no amount of gating the source helps a value
             * that has already been copied out of it. The iteration that
             * changes which screen is up must not also dispatch input to
             * the new one.
             *
             * The close path below already does this -- it ends in
             * `continue` -- which is why choosing a track never leaked a
             * tap onto the transport bar and only the open direction
             * showed the fault.
             */
            browser_draw();
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
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
                touch_swallow();        /* mirror image: see touch_swallow() */
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
                touch_swallow();        /* mirror image: see touch_swallow() */
                s_repaint_art = true;
                break;
            case BROWSER_CANCELLED:
                browser_close();
                touch_swallow();        /* mirror image: see touch_swallow() */
                s_repaint_art = true;
                break;
            default:
                break;
            }
            browser_draw();
            vTaskDelay(pdMS_TO_TICKS(bdown ? 20 : 100));
            continue;
        }

        /* Asked every repaint rather than cached on track change: the
         * chooser can load a different folder underneath, and a next
         * button greyed against a playlist that no longer exists is
         * worse than one that is briefly right for the wrong reason. */
        st.has_next = playlist_has_next(browser_order());

        st.title = s_tags.title[0] ? s_tags.title : s_display_name;
        st.artist = s_tags.artist;
        st.album = s_tags.album;
        st.playing = s_playing;
        st.volume = s_volume;
        st.muted = audio_out_muted();
        st.pos_sec = s_pos_sec;
        st.len_sec = s_len_sec;
        st.can_seek = s_can_seek;
        st.screen_off = s_screen_off;
        st.stats_valid = s_stats_valid;

        /*
         * Paused, or between tracks with nothing open -- shut the
         * amplifier down. Called every iteration and a compare when
         * unchanged; audio_out.c owns the hold that stops a track
         * boundary powering the output stage down and straight back up.
         *
         * The analog stage only. The USB port stays powered and a UAC
         * device stays enumerated, because re-enumerating on every play
         * press would cost seconds of silence to save power on a bus
         * that is feeding the headset anyway.
         */
        /*
         * Unchanged, and worth saying why it did not need changing.
         *
         * These two used to coincide: pause stopped the decode loop, so
         * !s_playing and a stalled decoder were the same event and the
         * amp went down once. They have come apart -- the decoder now
         * runs on through a pause until the ring is full -- but the
         * condition is an OR, so pause still idles the amp on the press
         * and the reason is now the s_playing half rather than the
         * s_decoding half. Same result, different term.
         *
         * The ordering is better than it was, too. The amp cuts as the
         * writer stops rather than up to a minute before it, which is the
         * gap the old behaviour played its buffer out into.
         */
        audio_out_set_idle(!s_playing || (!s_decoding && !s_track_changing));
        st.battery_pct = battery_pct();
        st.battery_charging = battery_charging();

        const bool down = bdown;
        ui_action_t act = ui_touch(&st, down, bx, by);

        /*
         * The remote, when the panel had nothing to say.
         *
         * Touch wins if both landed in the same iteration -- a finger on
         * the glass is the more deliberate of the two, and dispatching
         * both would be two actions from one pass, which is the thing
         * "one press is one action" exists to prevent.
         *
         * Deliberately not gated on screen_off: the point of a remote is
         * that it works with the player in a pocket. A volume key does
         * not wake the screen either, for the same reason.
         */
        if (act.kind == UI_ACTION_NONE && s_hid_action >= 0) {
            act.kind = (ui_action_kind_t)s_hid_action;
            act.value = s_hid_value;
            s_hid_action = -1;
        }

        /*
         * One line per press, at the point they are dispatched rather
         * than at each case, so a new action cannot be added and forget
         * to log itself.
         *
         * VOLUME is excluded because it is not a press: a drag emits one
         * every poll, fifty a second, and logging those buries
         * everything else. Its release is logged by the case below.
         */
        if (act.kind != UI_ACTION_NONE && act.kind != UI_ACTION_VOLUME) {
            if (act.kind == UI_ACTION_SEEK) {
                ESP_LOGI(TAG, "button: %s -> %d%%",
                         ui_action_name(act.kind), act.value);
            } else {
                ESP_LOGI(TAG, "button: %s", ui_action_name(act.kind));
            }
        }

        switch (act.kind) {
        case UI_ACTION_PLAY_PAUSE:
            s_playing = !s_playing;
            break;
        case UI_ACTION_VOLUME:
            s_volume = act.value;
            settings_set_volume((uint8_t)act.value);
            /* Moving the slider unmutes. Adjusting a control that is
             * suspended and hearing nothing is the kind of thing people
             * conclude is a broken player rather than a mute they
             * forgot; every hardware volume knob behaves this way. */
            if (audio_out_muted()) audio_out_set_mute(false);
            audio_out_set_volume((uint8_t)act.value);
            break;
        case UI_ACTION_MUTE:
            audio_out_set_mute(!audio_out_muted());
            break;
        case UI_ACTION_SEEK:
            request_seek(act.value, "slider");
            break;
        case UI_ACTION_PREV:
            /*
             * Back to the start of the track first, and only to the
             * previous track if it is already there.
             *
             * The threshold is 3 seconds because that is roughly how long
             * it takes to decide you meant the other one. It is the
             * behaviour of every physical transport and every player
             * since, and the reason is that "restart this" and "go back
             * one" are both wanted from the same button far more often
             * than either is wanted from its own.
             */
            /* Remembered before anything moves, so a second tap can ask
             * "before THIS one" regardless of what the first tap started
             * in the meantime. */
            snprintf(s_prev_anchor, sizeof(s_prev_anchor), "%s", s_path);

            if (s_len_sec > 0 && s_pos_sec >= 3) {
                request_seek(0, "prev: restart");
            } else {
                const char *p = playlist_prev();
                if (p) request_track(p);
                else   request_seek(0, "prev: first track");
            }
            break;

        case UI_ACTION_PREV_AGAIN: {
            /*
             * A second tap within the double-tap window: go back through
             * what was actually played, rather than back through the
             * list.
             *
             * Position is not consulted here. The 3-second rule exists to
             * disambiguate one press; a deliberate second press has
             * already said which of the two was meant, so applying the
             * rule again would make a double tap restart the track --
             * the one thing it certainly does not mean.
             *
             * The first tap of the pair has already acted, and under
             * PLAY_ORDER_ALL it usually did the same thing this will.
             * That is fine: it lands on the same track and the cache
             * makes the second arrival free. Under shuffle they differ,
             * which is the case worth having.
             */
            const char *h = history_back_from(s_prev_anchor);
            if (h) {
                ESP_LOGI(TAG, "back through history to %s", h);
                /* Keep the list pointing at where playback actually is,
                 * so the track after this one follows from here rather
                 * than from wherever the first tap left the index. */
                const int idx = playlist_index_of(h);
                if (idx >= 0) playlist_set_current(idx);
                request_track(h);
            } else {
                ESP_LOGI(TAG, "no play history; restarting the track");
                request_seek(0, "prev x2: no history");
            }
            break;
        }
        case UI_ACTION_NEXT: {
            /*
             * PLAY_ORDER_ONE means "do not go on by yourself" -- it is an
             * answer about what happens at the end of a track, and a
             * press of the next button is not the end of a track. Asking
             * playlist_next() with the order as-is would return NULL and
             * the button would do nothing, which reads as broken rather
             * than as a setting being respected.
             */
            const play_order_t ord = browser_order();
            const char *p = playlist_next(ord == PLAY_ORDER_ONE
                                          ? PLAY_ORDER_ALL : ord);
            if (p) request_track(p);
            else   ESP_LOGI(TAG, "no next track in %s", playlist_dir());
            break;
        }
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
        st.muted = audio_out_muted();
        st.pos_sec = s_pos_sec;
        st.len_sec = s_len_sec;
        st.can_seek = s_can_seek;
        st.screen_off = s_screen_off;
        st.stats_valid = s_stats_valid;
        st.battery_pct = battery_pct();
        st.battery_charging = battery_charging();
        ui_draw(&st);

        /* 50 Hz under a finger, 25 Hz while the title is travelling, 10 Hz
         * otherwise. The middle rate exists because a marquee stepped at
         * 10 Hz does not read as movement, it reads as a title jumping
         * three pixels at a time; ui_animating() is false the moment the
         * title fits, so a short one costs nothing. */
        int period = 100;
        if (down) period = 20;
        else if (!s_screen_off && ui_animating()) period = 40;
        vTaskDelay(pdMS_TO_TICKS(period));
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
    TRACK_ENDED = 0,    /* played to the end of the file */
    TRACK_INTERRUPTED,  /* the chooser picked something else */
    TRACK_MEDIA_GONE,   /* the card or drive was pulled */
    /*
     * Opened or decoded to nothing -- no audio came out of it.
     *
     * Split out of TRACK_ENDED because the caller treated the two
     * identically and therefore advanced, which is right for a finished
     * track and catastrophic for a folder on a card that has started
     * timing out: every open fails instantly, so the player sprints
     * through the entire playlist in under a second and lands at the end
     * of the folder having played nothing.
     *
     * A volume that is still mounted but not answering is not the same
     * as one that has been pulled, so this is not TRACK_MEDIA_GONE
     * either: the card is there, it is just failing, and it may well
     * come back.
     */
    TRACK_UNREADABLE,
} track_end_t;

static track_end_t play_file(const char *path)
{
    const storage_id_t vol = storage_of_path(path);

    /* Before the open, not after. decoder_open() on a Xing-less MP3 scans
     * the whole file to build a seek index, and until it returns the
     * screen would otherwise still be showing the track that just
     * ended. */
    const int64_t t_start = esp_timer_get_time();
    s_track_changing = true;
    track_change_begin(path);

    /*
     * Timed because the first flash put 15.4 s and 18.9 s between the
     * press and the sound, and nothing in the log attributed them. The
     * arbiter report immediately after closes the window on the open
     * alone: on a Xing-less MP3 it shows one playback reader pulling the
     * entire file, which is MP3D_SEEK_TO_SAMPLE building its index and
     * not, as it first looked, contention with anything.
     *
     * Two numbers, because they answer different questions. The
     * milliseconds are what the user waits. The KB are why.
     */
    /*
     * Reset the phase window so the open's percentage is measured against
     * the open.
     *
     * 0102's first track reported "14798 ms held of 20979 ms (70%)" and
     * the 70% was an artefact: the window ran from the previous report,
     * which for the first track is boot, so it had six seconds of
     * somebody reading the chooser in it. Held over open_ms was 99.2%,
     * the same as every later track. A denominator that silently includes
     * idle time makes the one number this was built to produce wrong in
     * the one case nobody can check by eye.
     */
    (void)storage_io_phase_ms();

    const int64_t t_open = esp_timer_get_time();
    decoder_t *dec = decoder_open(path);
    const uint32_t open_ms = (uint32_t)((esp_timer_get_time() - t_open) / 1000);
    if (!dec) {
        storage_io_report("failed open");
        return TRACK_UNREADABLE;
    }

    ESP_LOGI(TAG, "open took %" PRIu32 " ms", open_ms);
    storage_io_report("open");

    /*
     * Set after the open succeeds, not before: decoder_open() on a
     * Xing-less MP3 scans the whole file, and a seek accepted during
     * that scan would have no loop to reach for a long time -- which is
     * the bug this flag exists to prevent, in miniature.
     */
    s_decoding = true;

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
        /* heap_caps_malloc'd, so heap_caps_free'd. free() happens to work
         * for PSRAM in IDF today, but pairing the allocator is not
         * something to leave to happening-to-work on the one path that
         * only runs when memory is already in trouble. */
        heap_caps_free(pcm);
        heap_caps_free(st);
        decoder_close(dec);
        storage_hold(STORAGE_COUNT);
        /* Set just above, and there is no loop after this return. Left
         * true, every later seek would be accepted for a decode loop
         * that does not exist -- reintroducing the queued-forever bug on
         * the one path where it would be hardest to spot. */
        s_decoding = false;
        return TRACK_UNREADABLE;
    }

    uint32_t cur_rate = 0;
    int cur_chans = 0;
    int blocks = 0;
    uint64_t frames_out = 0;
    /* Cleared with the local it mirrors, so a new track cannot briefly
     * show the previous one's position. */
    s_frames_out = 0;
    s_frames_rate = 0;
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
    /* Published last, and only here. Everything the bar draws from is now
     * this track's; before this line it was the previous track's and the
     * UI was drawing dashes rather than pretending otherwise. */
    s_stats_valid = true;
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
            /* Hand it over rather than draw it. The envelope is part of
             * the transport bar now, so it is drawn by ui_draw() on every
             * repaint like everything else in there -- it has to be, it
             * changes colour as the track plays.
             *
             * That also retires the bubble recapture that used to follow
             * this: the artwork no longer changes when a scan lands, so
             * the saved strip behind the finger bubble is still good.
             */
            waveform_set(&s_walk);

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

        /*
         * No pause stall here any more; i2s_writer_task() holds it.
         *
         * The decode loop carries on filling the ring while paused and
         * then blocks in xStreamBufferSend() once it is full, which is
         * the same place it blocks during ordinary playback. Pausing now
         * leaves the ring full rather than draining it, so resume needs
         * nothing from the card.
         *
         * What was lost with the stall is the escape below it, which is
         * why that check moved up here rather than staying where it was:
         * a track chosen while paused used to break out of this loop
         * because the stall was watching for it. Nothing waits now, so
         * the check has to be reached on its own.
         */

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
            const uint32_t waited =
                (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount() - s_seek_asked);
            s_seek_pct = -1;

            /* Loud past a tenth of a second. Anything the decode loop
             * takes this long to notice will have been pressed again by
             * then, and the second press is what gets blamed. */
            if (waited > 100) {
                ESP_LOGW(TAG, "seek (%s) waited %" PRIu32 " ms for the decode loop",
                         s_seek_why, waited);
            }

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

        /*
         * Phase timing around the two calls that can block.
         *
         * The decode loop is where a control request is noticed, so
         * anything that stalls it is a button that appears not to work.
         * Which of the two it is matters: a slow decoder_read() is the
         * device, a slow send is the writer or the I2S clock, and from
         * the outside they are the same symptom. Only logged past the
         * threshold, so a healthy loop stays silent.
         */
        const TickType_t t_read = xTaskGetTickCount();

        decoder_info_t info;
        const int n = decoder_read(dec, pcm, DECODER_MAX_INT16, &info);
        if (n <= 0) break;

        const uint32_t read_ms =
            (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount() - t_read);
        if (read_ms > LOOP_STALL_MS) {
            ESP_LOGW(TAG, "decoder_read blocked %" PRIu32 " ms", read_ms);
        }

        if ((uint32_t)info.sample_rate != cur_rate || info.channels != cur_chans) {
            ESP_LOGI(TAG, "%s: %d Hz, %d ch, %d kbps",
                     info.codec, info.sample_rate, info.channels,
                     info.bitrate_kbps);

            /* For the format card, which is what a file with no picture
             * in it shows instead of a cover. Published rather than
             * fetched: media_task cannot ask the decoder anything -- the
             * decoder_t belongs to this loop, and handing it across is
             * the mistake s_ring_pct exists to avoid. */
            snprintf(s_fmt_codec, sizeof(s_fmt_codec), "%s",
                     info.codec ? info.codec : "");
            s_fmt_rate = info.sample_rate;
            s_fmt_chans = info.channels;
            s_fmt_kbps = info.bitrate_kbps;
            s_fmt_known = true;

            if (cur_rate == 0) {
                /* First block: set the rate before anything is queued,
                 * so the reconfigure never happens mid-stream. */
                ESP_ERROR_CHECK(audio_out_set_format((uint32_t)info.sample_rate, 2));
                HEAP_CHECK("before ring create");
                s_ring_pct = 0;
                /* Static: no allocation here, and the matching delete
                 * below frees nothing. If the boot allocation failed the
                 * player runs without a ring rather than not at all --
                 * checked below, because a NULL here is a silent hang. */
                s_pcm = s_pcm_store
                        ? xStreamBufferCreateStatic(PCM_RING_BYTES, PCM_CHUNK_BYTES,
                                                    s_pcm_store, &s_pcm_struct)
                        : NULL;
                if (!s_pcm) {
                    ESP_LOGE(TAG, "no PCM ring; cannot play");
                    why = TRACK_UNREADABLE;
                    break;
                }
                s_decode_done = false;
                s_writer_done = false;
                s_writer_stop = false;
                xTaskCreate(i2s_writer_task, "i2s_wr", 4096, NULL, 6, NULL);
            } else if ((uint32_t)info.sample_rate != cur_rate) {
                /* i2s_channel_reconfig_std_clock() needs the channel
                 * disabled, and doing that with audio queued is an
                 * audible click. Drain first. */
                /* Up to PCM_RING_BYTES of audio to play out first, which
                 * at 10 MB is a minute rather than the third of a second
                 * this was written for.
                 *
                 * Only reachable on a sample rate change inside one file,
                 * which no normal MP3 does -- but if one ever arrives,
                 * this is a minute of apparent hang with the transport
                 * unresponsive, not a pause. At the sizes this ring is
                 * being tested at, the drain wants replacing with a
                 * reset-and-accept-the-click before the size is made
                 * permanent. */
                while (!xStreamBufferIsEmpty(s_pcm)) vTaskDelay(1);
                ESP_ERROR_CHECK(audio_out_set_format((uint32_t)info.sample_rate, 2));
            }
            cur_rate = (uint32_t)info.sample_rate;
            cur_chans = info.channels;
        }

        /* The I2S slot config is stereo, so mono is duplicated into both
         * slots. Done here rather than in the backends so neither of them
         * has to know about the output format. */
        /*
         * Sent in slices with a timeout rather than one blocking call.
         *
         * The seek and track-change checks are at the top of this loop,
         * so the loop's worst-case latency IS the control latency -- and
         * a full ring means this call blocks until the writer has drained
         * enough for the whole block. That was tolerable at 64 KB (0.37 s)
         * and became four times worse the moment the ring went to 256 KB,
         * which is a control regression introduced by a change that was
         * about throughput and said nothing about buttons.
         *
         * Slicing decouples the two: the ring can be any size and a
         * press is still noticed within SEND_SLICE_MS. If a request is
         * already waiting, the rest of this block is abandoned -- it is
         * audio for a position that is about to be thrown away, and
         * finishing it only delays the jump the user asked for.
         *
         * Note that ring SIZE is not the problem here and enlarging it
         * did not cause one: the writer drains continuously, so a send
         * only ever waits for room for a single block -- about 27 ms at
         * 64 KB and about 27 ms at 512 KB. Slicing takes that 27 ms to
         * nearly zero, which is worth having and is not worth
         * attributing a multi-second delay to.
         */
        const uint8_t *src;
        size_t remain;
        if (info.channels == 1) {
            for (int i = 0; i < n; i++) {
                st[2 * i] = st[2 * i + 1] = pcm[i];
            }
            src = (const uint8_t *)st;
            remain = (size_t)n * 2 * sizeof(int16_t);
        } else {
            src = (const uint8_t *)pcm;
            remain = (size_t)n * sizeof(int16_t);
        }

        const TickType_t t_send = xTaskGetTickCount();
        while (remain) {
            if (s_seek_pct >= 0 || s_pending_ready) break;
            const size_t sent = xStreamBufferSend(s_pcm, src, remain,
                                                  pdMS_TO_TICKS(SEND_SLICE_MS));
            src += sent;
            remain -= sent;
        }
        ring_publish();

        const uint32_t send_ms =
            (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount() - t_send);
        if (send_ms > LOOP_STALL_MS) {
            ESP_LOGW(TAG, "ring send blocked %" PRIu32 " ms (ring %d%%)",
                     send_ms, ring_headroom_pct());
        }
        /* Position from samples decoded, not from bytes read: with VBR
         * the two disagree, and this is the number the slider shows.
         *
         * Published rather than turned into seconds here. The conversion
         * needs the ring's occupancy at the moment it is asked, and this
         * loop stops running a minute before the audio does -- see
         * pos_publish(). */
        frames_out += (uint64_t)(n / (info.channels > 0 ? info.channels : 1));
        s_frames_rate = cur_rate;
        s_frames_chans = info.channels > 0 ? (uint32_t)info.channels : 1;
        s_frames_out = frames_out;

        /*
         * The number the listener actually experiences: press to sound,
         * not open to sound. It spans track_change_begin(), the open and
         * the first decode, because that is the span during which the
         * screen is blank and nothing is playing.
         *
         * Logged once per track, on the first block only.
         */
        if (blocks == 0) {
            /* The ring gauge at the moment sound starts. The decode loop
             * has had the whole open to fill it and the writer has not
             * started draining, so a low number here is the decoder
             * losing a race it began with a head start -- which is the
             * shape a gapless prefetch has to fit into, and the reason
             * this is worth a line rather than being inferred later from
             * the absence of an underrun. */
            ESP_LOGI(TAG, "first sound %" PRIu32 " ms after the press "
                     "(open was %" PRIu32 " ms of it), ring %d%%",
                     (uint32_t)((esp_timer_get_time() - t_start) / 1000),
                     open_ms, ring_headroom_pct());
        }

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
        /* Release the writer from any pause before waiting on it. Both
         * of the spins below need it to run, and neither can be the
         * thing that unpauses it. */
        s_writer_stop = true;

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
        /*
         * Order matters. -1 is published FIRST, so anything asking about
         * occupancy is already being told there is no ring before the
         * memory stops existing. Then the handle is cleared, then the
         * buffer is freed -- so at no point is there a reachable handle
         * to a freed buffer.
         *
         * Plain create, plain delete: the allocator pairing that the
         * WithCaps version had to get right is simply not present now.
         */
        HEAP_CHECK("before ring delete");
        s_ring_pct = -1;
        StreamBufferHandle_t doomed = s_pcm;
        s_pcm = NULL;
        vStreamBufferDelete(doomed);
    }
    /* No loop from here on, so no seek can be serviced. Cleared before
     * the log line rather than after, so nothing can slip in between. */
    s_decoding = false;
    /* Cleared here rather than at the first decoded block, so that a
     * track which fails to open cannot leave the amplifier powered for
     * ever waiting for a sound that is not coming. */
    s_track_changing = false;
    if (s_seek_pct >= 0) {
        ESP_LOGI(TAG, "dropping unserviced seek (%s): track over", s_seek_why);
        s_seek_pct = -1;
    }

    /* Why, not just that. "finished" on a track the user skipped out of
     * reads as the skip having been ignored until the next line arrives. */
    /* A track that ran to the end without producing a single block did
     * not play. Reported as unreadable so the caller can count it, since
     * "finished, 0 blocks" and "finished, 261 blocks" mean opposite
     * things and only one of them is a reason to move on cheerfully. */
    if (why == TRACK_ENDED && blocks == 0) why = TRACK_UNREADABLE;

    /* Why, not just that. "finished" on a track the user skipped out of
     * reads as the skip having been ignored until the next line arrives. */
    ESP_LOGI(TAG, "%s, %d blocks",
             why == TRACK_INTERRUPTED ? "interrupted"
             : why == TRACK_MEDIA_GONE ? "media gone"
             : why == TRACK_UNREADABLE ? "unreadable"
                                       : "finished", blocks);

    /* Closes the window the open's report opened. Everything here is
     * steady-state playback plus whatever background work got in
     * alongside it, which is the comparison worth having: playback's
     * worst wait in this window is the arbiter's actual result, while
     * the same figure in the "open" window is measured against an
     * otherwise idle card. */
    storage_io_report("track");

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
    int fails = 0;      /* consecutive tracks that produced no audio */
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
        history_push(s_path);
        const track_end_t why = play_file(s_path);
        have = false;

        if (why == TRACK_INTERRUPTED) continue;    /* s_pending has the next */

        if (why == TRACK_MEDIA_GONE) {
            /* The list points at paths on a volume that is no longer
             * there. Keeping it would offer a next track that cannot be
             * opened. */
            playlist_clear();
            /* The cache is keyed by path and every path in it is on the
             * volume that just left. Keeping them would mean a later
             * track with a coincidentally equal path getting someone
             * else's cover. */
            mediacache_clear();
            s_open_chooser = true;
            continue;
        }

        /*
         * Stop after a run of tracks that produced no audio.
         *
         * One unreadable file in a folder is a bad rip and skipping it is
         * the right answer. Several in a row is not a property of the
         * files -- it is the card, and continuing means racing to the end
         * of the folder in under a second, logging twenty-two "playing"
         * lines for tracks nobody heard, and arriving at the chooser with
         * the playlist exhausted and no indication of why.
         *
         * Three, because a folder can legitimately begin with a couple of
         * duds and because three failures is already a tenth of a second
         * when they are failing instantly.
         */
        if (why == TRACK_UNREADABLE) {
            if (++fails >= UNREADABLE_LIMIT) {
                ESP_LOGE(TAG, "%d tracks in a row played nothing; stopping. "
                              "The volume is mounted but not readable.", fails);
                s_open_chooser = true;
                fails = 0;
                continue;
            }
            ESP_LOGW(TAG, "%s played nothing; skipping", s_path);
        } else {
            fails = 0;
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

    /* I2S, the codec, the speaker amp and the headphone-detect task, all
     * behind one call now -- see audio_out.h. 44100 is only what MCLK
     * starts at; the first track states its own. */
    ESP_ERROR_CHECK(audio_out_init(s_i2c_bus, s_exp1, 44100));

    /*
     * The PCM ring's storage, once, for the life of the program. One
     * byte over PCM_RING_BYTES because that is what the static variant
     * requires to tell a full ring from an empty one.
     *
     * Not fatal on failure: play_file() reports the track unreadable and
     * the chooser comes up, which is a worse player than one with a ring
     * and a better one than a boot loop.
     */
    s_pcm_store = heap_caps_malloc(PCM_RING_BYTES + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_pcm_store) {
        ESP_LOGE(TAG, "could not reserve %u KB for the PCM ring",
                 (unsigned)(PCM_RING_BYTES / 1024));
    }

    /* Both volumes, and the poll task that keeps them up to date. Not
     * ESP_ERROR_CHECK'd on the media itself: an empty slot and an empty
     * port are a normal way to boot now, and the chooser says so on
     * screen. */
    /* The bus owner before anything that registers a class driver with
     * it: registration is refused once the port is up, and storage_init()
     * registers one. */
    ESP_ERROR_CHECK(usbhost_init(s_exp2));

    /*
     * Before storage_init(), because settings.c reads through the same
     * arbiter and the first mount happens in there. It degrades to plain
     * stdio if it is ever reached before this, which is the reason
     * nothing here is ESP_ERROR_CHECK()ed: an unarbitrated player is
     * what this replaced, and that one played.
     */
    storage_io_init();

    /* Once per boot, next to the arbiter line, because "why is there no
     * waveform" is otherwise answered only by reading CMakeLists. */
    if (!WAVEFORM_SCAN) {
        ESP_LOGI(TAG, "waveform scan disabled at build time "
                      "(idf.py -DWAVEFORM=1 build to re-enable)");
    }

    ESP_ERROR_CHECK(storage_init());

    /*
     * After storage, because it needs a mounted volume to read from, and
     * before anything asks for a setting. The volume it loads has to be
     * pushed at audio_out.c explicitly: audio_out_init() has already run
     * and left the codec at its own default of 50.
     */
    settings_init();
    s_volume = settings_volume();
    audio_out_set_volume((uint8_t)s_volume);

    /* The other class driver on that port. Not ESP_ERROR_CHECK'd on the
     * device: no headset plugged in is the normal way to boot, and the
     * analog path is what plays until one is. */
    if (uac_init() != ESP_OK) {
        ESP_LOGW(TAG, "no USB audio support this boot; analog output only");
    }

    /* The remote on the same headset. Also not fatal: a device with no
     * HID interface, or a HID interface that will not claim, leaves the
     * on-screen controls doing everything they already did. */
    if (hid_init(hid_button) != ESP_OK) {
        ESP_LOGW(TAG, "no USB remote support this boot");
    }

    /*
     * And power it, unconditionally.
     *
     * The old rule was that the port came up when there was no card at
     * boot, or when the USB tab was tapped. Both are questions about
     * where the files are, and a USB audio device is not a file source.
     * It also cannot announce itself through a dark port, so under the
     * old rule a headset plugged into a player with a working card was
     * invisible until somebody went looking for storage.
     *
     * Asynchronous: three I2C writes and a 100 ms inrush settle on the
     * bus task, so this returns immediately and the rest of boot carries
     * on. Still one-way -- see usbhost.h.
     */
    usbhost_start();

    /* The gauge is on the same bus as everything else and is allowed to
     * be absent: a board with no pack, or one wired differently, gets an
     * empty outline on the volume row and nothing else changes. Not
     * ESP_ERROR_CHECK'd for exactly that reason. */
    if (battery_init(s_i2c_bus) == ESP_OK) {
        ESP_ERROR_CHECK(battery_start());
    }

    /* Lowest priority in the program. It reads a whole file off the same
     * card the decoder is reading, and the decoder winning every time is
     * the correct outcome. */
    /* Priority 1, below the UI at 4 and well below the decoder. Nothing
     * this task produces is worth a millisecond of the audio path. */
    mediacache_init();
    xTaskCreate(media_task, "media", 8192, NULL, 1, NULL);

    /* Touch after the panel, always: TP_RST and LCD_RST are released by
     * the same expander write, so before panel_init() there is nothing on
     * the bus to talk to. */
    if (touch_init(s_i2c_bus, LCD_H_RES, LCD_V_RES) != ESP_OK) {
        ESP_LOGW(TAG, "no touch -- playing through with no controls");
    }
    ESP_ERROR_CHECK(ui_init(s_panel, LCD_H_RES, LCD_V_RES));
    /* 4 KB was enough when this task only drew the bar. The chooser runs
     * on it too now, and that reaches opendir()/readdir() through FatFs
     * and carries a couple of 512-byte path buffers on the way -- so the
     * old size overflowed on the first folder with a long name in it. */
    xTaskCreate(ui_task, "ui", 8192, NULL, 4, NULL);

    /*
     * Above media_task before the decode loop starts.
     *
     * player_loop() runs on main_task, and main_task's priority is 1 --
     * the same as media_task's. The comment on MEDIA_START_DELAY_MS
     * described media_task as "the lowest priority task in the program",
     * which was true of every task except the one it competes with.
     *
     * Equal priority means the scheduler round-robins them, so a
     * background walk reading 32 KB at a time gets every other turn at
     * the card against the decoder that is trying to keep the ring fed.
     * At 40 MHz that mostly kept up. At 20 MHz it does not, and it shows
     * as decoder_read blocked for three to twelve seconds while an
     * envelope scan runs.
     *
     * 5 rather than 4: a starved decoder is a dropout and a starved UI
     * is a slow button, and the decode loop blocks on the card anyway,
     * so it yields the CPU for the whole time it is not needed.
     *
     * This does not make the card any faster and does not replace the
     * time-and-depth throttle above -- priority still says nothing about
     * who gets the device. It makes the two tasks unequal, which is what
     * the throttle assumed all along.
     */
    vTaskPrioritySet(NULL, 5);

    /* Does not return. The volumes are never unmounted from here any
     * more; storage.c owns that, and it unmounts on removal rather than
     * on the end of a track. */
    player_loop();
}
