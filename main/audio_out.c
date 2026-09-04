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
 * The routing rule this file now implements is one line long: if a USB
 * audio device is attached and can take the format, it wins. See the
 * comment above arbitrate().
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio_out.h"
#include "battery.h"
#include "uac.h"

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

/*
 * How long the player has to have been idle before the amplifier is
 * actually shut down.
 *
 * The gap between two tracks is a gap in which nothing is being written,
 * and it is a few hundred milliseconds at most. Reacting to it would
 * power the output stage down and straight back up between every pair of
 * tracks on an album -- an audible click on a boundary that gapless
 * playback exists to make silent, which is a poor trade for a second and
 * a half of amplifier.
 *
 * Only the shutdown waits. Coming back is immediate: a deferred wake
 * would clip the start of whatever just started.
 */
#define IDLE_HOLD_MS            (1500)

/* ES8388 DACCONTROL3, bit 2 = DACMute. Not written by es8388_init(),
 * which leaves it at its reset default of unmuted. */
#define ES8388_REG_DACCONTROL3  (25)
#define ES8388_DACMUTE_BIT      (1 << 2)

/*
 * How long uac_write() will wait for room in the device's ring.
 *
 * A USB frame is 1 ms and the driver's ring holds about 93 ms, so a
 * healthy stream never comes near this. It is a stall detector, not a
 * flow control: past this the device is not draining and the honest
 * thing is to drop the block and say so rather than to stall the writer
 * task, which is the task the transport buttons are waiting behind.
 */
#define UAC_WRITE_TIMEOUT_MS    (200)

/* Software gain works a chunk at a time out of this rather than in
 * place, because the buffer handed to audio_out_write() belongs to the
 * caller and scaling it in place would quietly modify the ring's data.
 * 4 KB matches PCM_CHUNK_BYTES; a larger block is split. */
#define GAIN_SCRATCH_BYTES      (4 * 1024)

/* The enum lives in audio_out.h now, because the UI draws the route. The
 * short names stay as aliases: arbitrate() and analog_set() read better
 * with them, and renaming every use inside this file would bury the one
 * thing that actually changed under fifty lines of noise. */
typedef audio_out_route_t route_t;
#define ROUTE_SPEAKER     AUDIO_OUT_SPEAKER
#define ROUTE_HEADPHONES  AUDIO_OUT_HEADPHONES
#define ROUTE_USB         AUDIO_OUT_USB

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_exp1;
static i2c_master_dev_handle_t s_es8388;
static i2s_chan_handle_t       s_tx;

static volatile bool s_headphones;
static uint32_t s_rate;
static uint8_t  s_channels;
static uint8_t  s_volume = 50;

static volatile route_t s_route = ROUTE_SPEAKER;
static volatile bool    s_muted;

/*
 * s_idle is what the output stage has been told; s_idle_want is what the
 * player says. They differ only while the hold below is running.
 */
static volatile bool s_idle_want;
static volatile bool s_idle;
static TickType_t    s_idle_since;

/* The last uac.c generation this file has reacted to. Watched rather
 * than uac_present() polled, so a headset unplugged and replugged
 * between two blocks is noticed as a change rather than as "still
 * there". */
static uint32_t s_uac_seen;

/* The slider has to keep working whatever the device offers, so when
 * there is no control the driver can reach the gain is applied to the
 * samples on the way out.
 *
 * Asked per block rather than latched at route change: the answer
 * arrives asynchronously now -- uac_set_volume() publishes and the UAC
 * event task performs the transfer -- so the route can be taken before
 * the first attempt has been made. A volatile read is cheaper than being
 * wrong for the first second of every track. */
static int16_t *s_scratch;

static inline bool soft_gain(void)
{
    /*
     * Muted always takes the software path, whatever the device offers.
     *
     * "Set the device volume to zero" is not the same statement as
     * silence: the control is a feature-unit setting whose bottom step
     * is whatever the device decided it is, and on more than a few parts
     * that is quiet rather than nothing. Zeroing the samples is the only
     * version of mute that cannot be argued with, and it costs one pass
     * over a 4 KB block.
     */
    if (s_route != ROUTE_USB) return false;
    return s_muted || !uac_has_volume_control();
}

/* The jack's poll task decides nothing on its own any more -- it feeds
 * one input into arbitrate(), which is defined below it because it needs
 * the codec helpers. */
static void arbitrate(void);
static void analog_set(bool enabled);
static void idle_apply(bool idle);

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

/*
 * The highest Fs this output stage can be clocked at.
 *
 * MCLK is 256 x Fs for the ES8388, so 96 kHz asks for 24.576 MHz and
 * the P4's I2S clock divider refuses:
 *
 *   E i2s_std: i2s_std_calculate_clock(68): sample rate is too large
 *
 * 48 kHz is the highest that divides cleanly here, and it covers every
 * consumer format this player is likely to meet. A file above it is
 * refused rather than played at the wrong speed -- resampling is a
 * bigger feature than this line, and half-speed audio is a worse answer
 * than a skipped track and a log line.
 */
#define I2S_MAX_RATE_HZ         (48000)

/* The rate the output stage is currently clocked at, so a caller can
 * tell a reconfigure from a no-op before committing to one. */
uint32_t audio_out_rate(void) { return s_rate; }

bool audio_out_rate_supported(uint32_t rate)
{
    return rate > 0 && rate <= I2S_MAX_RATE_HZ;
}

static esp_err_t i2s_set_rate(uint32_t rate)
{
    if (!audio_out_rate_supported(rate)) return ESP_ERR_NOT_SUPPORTED;

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
            ESP_LOGI(TAG, "headphones %s", plugged ? "in" : "out");
            /* Not speaker_set() directly. The jack is one input to the
             * routing decision rather than the whole of it now: with a
             * USB device playing, unplugging the headphones must not
             * switch the speaker back on underneath it. */
            arbitrate();
            if (s_route != ROUTE_USB) analog_set(true);
        }
        /* The deferred half of audio_out_set_idle(). Signed tick
         * difference, not "now > then": the counter wraps every 49 days
         * at 1 kHz and the naive form is wrong once per wrap, on a
         * device people leave running. Same reasoning as
         * TOUCH_SETTLE_MS in touch.c. */
        if (s_idle_want && !s_idle &&
            (int32_t)(xTaskGetTickCount() - s_idle_since) >=
                (int32_t)pdMS_TO_TICKS(IDLE_HOLD_MS)) {
            idle_apply(true);
        }

        vTaskDelay(pdMS_TO_TICKS(HP_POLL_MS));
    }
}

audio_out_route_t audio_out_route(void)
{
    return s_route;
}

const char *audio_out_route_name(void)
{
    switch (s_route) {
    case ROUTE_USB:        return "USB audio";
    case ROUTE_HEADPHONES: return "headphones";
    default:               return "speaker";
    }
}

/* ------------------------------------------------------------------ */
/* Routing                                                             */
/* ------------------------------------------------------------------ */

/*
 * Everything analog off, or back on according to the jack.
 *
 * Two things, not one. The amp enable alone is not enough: with
 * headphones in and SPK_EN already low, cutting only the amp leaves the
 * ES8388 driving OUT1 and the jack playing the same track the USB
 * headset is playing, a few milliseconds apart. So the DAC is muted as
 * well, which covers both outputs at once and is one I2C write.
 *
 * The I2S channel is deliberately left running and at the right rate.
 * Stopping it would drop MCLK, and the ES8388 stops answering on I2C
 * without MCLK -- so unmuting on the way back would be a codec
 * re-init rather than a register write. It costs a clock running into a
 * muted DAC, which is the state the part is in between tracks anyway.
 */
static void analog_set(bool enabled)
{
    /* Muted, idle and "USB has the route" are the same silence: the DAC
     * is muted and the amp is off. One condition rather than three
     * states, because each has to survive the others changing -- a mute
     * applied while USB is playing has to outlast the device being
     * unplugged, and it does, because this is re-run from arbitrate() on
     * the way back. */
    const bool on = enabled && !s_muted && !s_idle;
    reg_write(s_es8388, ES8388_REG_DACCONTROL3, on ? 0x00 : ES8388_DACMUTE_BIT);
    speaker_set(on && !s_headphones);
}

/*
 * The rule: a USB audio device that can take the format wins.
 *
 * It outranks the headphone jack, which outranks the speaker, and it
 * does so unconditionally rather than as a preference the user sets.
 * The argument is that plugging a USB DAC or headset into a device is
 * not an ambiguous act -- nobody connects one and then expects the
 * built-in speaker to keep playing -- and it is exactly the argument the
 * jack already wins on. The jack has had this behaviour since the first
 * version; this adds a rung above it rather than a new kind of rule.
 *
 * "Can take the format" is doing real work in that sentence. There is no
 * resampler here, so a device that only offers 48 kHz is not an output
 * for a 44.1 kHz file, and the correct thing is to fall back to the
 * analog path rather than to play it 9% fast. That decision is per
 * format, so it is re-made on every track: an album of 44.1 kHz files
 * with one 48 kHz track in it will route to USB, drop to the speaker for
 * that track, and go back.
 *
 * Called from audio_out_set_format() and from audio_out_write() when the
 * generation moves, so a headset plugged in mid-track is picked up at
 * the next block rather than at the next track.
 */
static void arbitrate(void)
{
    s_uac_seen = uac_generation();

    route_t want = s_headphones ? ROUTE_HEADPHONES : ROUTE_SPEAKER;

    if (uac_present() && s_rate && s_channels) {
        const esp_err_t err = uac_stream_start(s_rate, s_channels);
        if (err == ESP_OK) {
            want = ROUTE_USB;
        } else if (err == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGI(TAG, "USB device cannot take %lu Hz %u ch; staying analog",
                     (unsigned long)s_rate, s_channels);
        }
    }

    if (want == s_route) return;

    if (want == ROUTE_USB) {
        analog_set(false);
        /* Published, not performed. The device's own control is tried on
         * the UAC event task; until it answers, soft_gain() reports true
         * and the samples are scaled instead. Either way the slider
         * works from the first block. */
        uac_set_volume(s_volume);
    } else if (s_route == ROUTE_USB) {
        uac_stream_stop();
        analog_set(true);
        es8388_set_volume(s_volume);
    }

    s_route = want;
    /* The pack voltage on every route change. A USB device is the only
     * load here that is switched on and off by a routing decision, so a
     * rail that sags when one starts playing shows up as a step between
     * two of these lines and as nothing at all without them. */
    ESP_LOGI(TAG, "output: %s (pack %d mV)", audio_out_route_name(),
             battery_mv());
}

/*
 * Scale a block by the volume percentage, into the scratch buffer.
 *
 * Linear in amplitude rather than in dB, which is the wrong curve for a
 * volume control and is the same wrong curve the ES8388 path uses --
 * es8388_set_volume() maps a percentage linearly onto the mixer's
 * register steps. Matching it is the point: the slider should not feel
 * different depending on what is plugged in. If that curve is ever
 * fixed, both of these change together.
 *
 * Returns the number of samples written, always the number asked for.
 */
static void apply_gain(const int16_t *src, int16_t *dst, size_t samples, uint8_t percent)
{
    const int32_t g = (int32_t)percent;
    for (size_t i = 0; i < samples; i++) {
        dst[i] = (int16_t)((src[i] * g) / 100);
    }
}

/* ------------------------------------------------------------------ */

esp_err_t audio_out_set_format(uint32_t rate, uint8_t channels)
{
    if (rate == 0 || channels == 0) return ESP_ERR_INVALID_ARG;

    if (rate != s_rate) {
        /* Set even when the USB path is about to win it. The fallback
         * has to be instant -- a device unplugged mid-track re-routes at
         * the next block, and reconfiguring the I2S clock there would
         * mean disabling the channel with audio in flight. One reconfig
         * per track buys a fallback that is a route change and nothing
         * else. */
        ESP_RETURN_ON_ERROR(i2s_set_rate(rate), TAG, "rate");
    }
    s_rate = rate;
    s_channels = channels;

    arbitrate();
    return ESP_OK;
}

esp_err_t audio_out_write(const void *data, size_t len)
{
    /* A plug event between blocks. Cheap enough to test every time: it
     * is a load and a compare, and the alternative is a headset that
     * does not take over until the next track. */
    if (uac_generation() != s_uac_seen) arbitrate();

    if (s_route != ROUTE_USB) {
        size_t written = 0;
        return i2s_channel_write(s_tx, data, len, &written, portMAX_DELAY);
    }

    const uint8_t *src = (const uint8_t *)data;
    size_t remain = len;
    while (remain) {
        size_t n = remain;
        const void *out = src;

        if (soft_gain() && s_scratch) {
            if (n > GAIN_SCRATCH_BYTES) n = GAIN_SCRATCH_BYTES;
            apply_gain((const int16_t *)src, s_scratch, n / sizeof(int16_t),
                       s_muted ? 0 : s_volume);
            out = s_scratch;
        }

        const esp_err_t err = uac_write(out, n, UAC_WRITE_TIMEOUT_MS);
        if (err == ESP_ERR_INVALID_STATE) {
            /* The device went away between the generation check and the
             * write, which is the ordinary way a headset is unplugged.
             * Re-route and put the rest of this block out of whatever
             * answered. */
            arbitrate();
            if (s_route != ROUTE_USB) {
                size_t written = 0;
                return i2s_channel_write(s_tx, src, remain, &written, portMAX_DELAY);
            }
            return err;
        }
        if (err != ESP_OK) {
            /* A timeout is a device that has stopped draining. The block
             * is dropped rather than retried: the writer task is what
             * the transport buttons are queued behind, and a stalled
             * device must not become a dead play button. */
            ESP_LOGW(TAG, "USB output dropped %u bytes (%s)",
                     (unsigned)n, esp_err_to_name(err));
            return err;
        }

        src += n;
        remain -= n;
    }
    return ESP_OK;
}

esp_err_t audio_out_set_volume(uint8_t percent)
{
    if (percent > 100) percent = 100;
    s_volume = percent;

    /*
     * Both, unconditionally, and neither blocks.
     *
     * uac_set_volume() is two stores now; it used to take the UAC lock
     * and perform a USB control transfer, on this task -- which is the
     * UI task, dispatching every other button in the same loop. A drag
     * put fifty of those a second behind the mutex the audio writer
     * holds for the length of a write.
     *
     * The codec is set even while USB has the route, so a device
     * unplugged mid-track falls back at the right level rather than at
     * whatever it was when the device arrived.
     */
    uac_set_volume(percent);
    return es8388_set_volume(percent);
}

bool audio_out_muted(void) { return s_muted; }

void audio_out_set_mute(bool muted)
{
    s_muted = muted;

    /* The USB path needs nothing said to it: soft_gain() reads s_muted
     * on the way past and apply_gain() scales by zero. The analog path
     * is two register writes and they have to happen now rather than at
     * the next route change. */
    analog_set(s_route != ROUTE_USB);

    ESP_LOGI(TAG, "%s (%s)", muted ? "muted" : "unmuted", audio_out_route_name());
}

/* The analog stage only. Nothing here touches the USB port: a paused
 * player keeps its UAC device enumerated and its stream open, because
 * re-enumerating on every play press would cost seconds of silence to
 * save power on a bus that is powering the headset anyway. */
static void idle_apply(bool idle)
{
    if (idle == s_idle) return;
    s_idle = idle;
    analog_set(s_route != ROUTE_USB);
    ESP_LOGI(TAG, "amplifier %s", idle ? "off (idle)" : "on");
}

void audio_out_set_idle(bool idle)
{
    if (idle == s_idle_want) return;
    s_idle_want = idle;

    if (!idle) {
        /* Immediately, and on the caller's task. Two I2C writes, and the
         * alternative is up to HP_POLL_MS of a track's opening bar
         * played into a disabled amplifier. */
        idle_apply(false);
        return;
    }
    /* Going quiet waits out IDLE_HOLD_MS on the poll task below. */
    s_idle_since = xTaskGetTickCount();
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

    /* Internal rather than PSRAM: it is 4 KB, it is touched once per
     * block on the task that must not stall, and PSRAM is already
     * carrying the shadow framebuffer and the cover cache. */
    s_scratch = heap_caps_malloc(GAIN_SCRATCH_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_scratch) {
        /* Not fatal. Without it a device with no volume control plays at
         * full scale and the slider does nothing, which is worse than
         * this line and better than not booting. */
        ESP_LOGW(TAG, "no gain scratch buffer; USB volume will be fixed");
    }

    if (xTaskCreate(headphone_task, "hp_det", 3072, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
