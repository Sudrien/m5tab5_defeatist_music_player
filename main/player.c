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
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
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
#include "player_diag.h"
#include "covertag.h"
#include "heapcheck.h"
#include "mediacache.h"
#include "browser.h"
#include "decoder.h"
#include "framewalk.h"
#include "loudness.h"
#include "replaygain.h"
#include "hid.h"
#include "panel.h"
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
#define LCD_BITS_PER_PIXEL      (16)
#define DSI_DATA_LANES          (2)
/*
 * Sized to the pixel clock with a margin, not set as high as the PHY
 * will go. See the note below on why that is not the same thing.
 *
 *   720 x 1280 RGB565 at DPI_CLOCK_MHZ = 70 MHz
 *   -> 70 MHz x 16 bits / 2 lanes = 560 Mbps per lane required
 *   -> 700 is 25% over, against Espressif's recommended ~20%
 *
 * Documented floor is 480 and the ceiling on this part is 1500.
 */
#define DSI_LANE_RATE_MBPS      (700)
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
 *   - DSI_LANE_RATE_MBPS is sized to this, and the reasoning that used
 *     to be here -- that the lane rate is the wire, not the memory, so a
 *     bigger margin costs nothing -- is wrong in the way that matters.
 *     Average bandwidth, yes. Burstiness, no: the bridge drains its line
 *     buffer at wire speed and then waits, so the faster the wire the
 *     more the fetch looks like a spike rather than a stream, and a
 *     spike is what a fetch that cannot wait underruns on. Espressif's
 *     LCD FAQ lists a lane rate mismatched to the pixel clock as a cause
 *     of this exact blue-screen symptom and recommends sizing it about
 *     20% above what the clock demands. 965 against a demand of 560 was
 *     72% above.
 *
 *     So the two move together. If the pixel clock comes down, work the
 *     lane rate out again: pixel clock x 16 bits / 2 lanes, plus a
 *     fifth, against a documented floor of 480. If a low rate ever gives
 *     a sheared or misaligned picture rather than a black one, that is
 *     the mismatch to go and look at.
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

/*
 * DSI bridge underruns, counted in the ISR.
 *
 * Defined in the vendored components/esp_lcd -- see cmake/dpi_instrument.cmake
 * and tools/instrument_dpi.sh, which replace that ISR's ESP_DRAM_LOGE with
 * an increment of this. The log call was never seen once in any capture,
 * and there was no way to tell whether that meant no underruns or a
 * printf that never reached the USB console. This tells them apart.
 *
 * Read without a lock. It is a single aligned word written by one ISR and
 * read by one task, and a report that is one behind is a report about the
 * same second.
 */
extern uint32_t g_tab5_dpi_underruns;

/*
 * DIAGNOSTIC. Not a feature, and not meant to survive.
 *
 * 0501 established that the cyan flash is not a DSI bridge underrun: the
 * counter above stays at zero across a flashing seek. That excludes the
 * mechanism every patch from 0403 to 0500 was aimed at, and leaves two
 * halves of the seek unexamined -- the audio work (decoder_seek_sec()
 * and the ring reset that follows it) and everything else the release of
 * a drag sets in motion.
 *
 * Set to 1, the seek is serviced in every respect except the two calls
 * that touch audio: the request is taken, the loudness measurement is
 * dropped, the wait is timed, the position counter is re-anchored and
 * the bar moves. The decoder is not seeked and the ring is not emptied,
 * so playback continues from where it was.
 *
 * If the flash still happens, nothing in the audio path is involved and
 * the cause is in the touch-release or UI path, which nothing in this
 * investigation has looked at yet. If it stops, it is inside
 * decoder_seek_sec() -- and since 0500 excluded the refill that follows,
 * that means mp3dec_ex_seek() itself: the index lookup, the backward
 * fseek, and the frames it decodes to prime the bit reservoir.
 *
 * The clock will read wrong and the audio will not move while this is
 * set. That is the point; it is one build, not a mode.
 *
 * 0503 turns it back off. The answer it gave was 'fewer flashes',
 * which is not an answer -- and the same capture showed the volume
 * moving on its own seven times, which means some of those seek
 * releases were not presses at all. The experiment cannot be read
 * until the touch trace says which events were real.
 */
#define SEEK_NOOP               (0)

/*
 * The other half of the same bisection.
 *
 * 0505 retired the supply: fifteen traces across a seek commit, 15-21 mV
 * of spread every time, the minimum landing anywhere in the window. A
 * load step would have clustered at the commit. Nothing does.
 *
 * So the flash is inside the commit, and the commit is two calls.
 * decoder_seek_sec(), which is mp3dec_ex_seek() -- an index lookup, a
 * backward fseek, and the frames minimp3 decodes to prime the bit
 * reservoir. And xStreamBufferReset(), which drops the queued PCM.
 *
 * 0500 showed the refill AFTER the reset is not the cause. It did not
 * show the reset itself is not: emptying a 3.5 MB stream buffer walks
 * its control block and leaves the writer, which may be parked inside
 * xStreamBufferReceive() on that same buffer, to be released and rerun.
 *
 * Set to 1, the decoder is seeked and the ring is left alone. The new
 * position decodes into a ring that still holds up to twenty seconds of
 * the old one, so the jump is heard late -- which is the exact behaviour
 * the reset was added to remove, and is why this is a diagnostic and not
 * an option.
 *
 * Still flashes: it is mp3dec_ex_seek(), and SEEK_NOOP's "fewer" was
 * measuring that.
 *
 * Stops flashing: it is the ring reset, and the two experiments together
 * bracket it -- 0500 excluded what happens after, this excludes
 * everything else.
 *
 * Run it against a counted ten seeks, not an impression. Every reading
 * in this investigation that rested on one has since been wrong.
 */
#define SEEK_KEEP_RING          (0)
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
/*
 * 20 SECONDS, NOT SIXTY -- step one of the two-ring conversion.
 *
 * 10 MB is 59 s of 44.1 kHz stereo. That was chosen to swallow an
 * eleven-second card read and it does, but it made every threshold
 * expressed as a fraction of the ring mean something absurd:
 * MEDIA_MIN_RING_PCT at 60% of 59 s is 34 seconds of audio, demanded
 * inside an 8000 ms window. That is 4.3x realtime. A card supplies it
 * and a USB drive does not, which is the whole of
 *
 *   W cover: ring never reached 60% in 8000 ms; going ahead anyway
 *   W prefetch: ring never reached 75% in 8000 ms; going ahead anyway
 *
 * -- not a throughput problem, a threshold measured against a ring that
 * grew 160x without the thresholds being revisited. At 3.5 MB, 60% is
 * 12 s of audio in 8 s, or 1.5x realtime, which both volumes clear.
 *
 * Two of these is 7 MB against the 10 MB one, so the conversion to a
 * pair of rings costs no PSRAM. This patch does not make that pair --
 * the ring and i2s_writer_task are still created and destroyed inside
 * play_file(), and a track boundary is still "drain, stop the writer,
 * delete" -- but it settles the size first, because that boundary is
 * built around the drain and the drain is where the size hurts.
 *
 * What it gives up: slack. Worst wait was 0 ms with 59 s banked and the
 * settings write held the volume for 1005 ms; 20 s still covers that
 * with room to spare, but an eleven-second card stall is now 20 s of
 * cover rather than 59 s. If those come back this is the first number
 * to look at.
 */
#define PCM_RING_BYTES          (3520 * 1024)       /* ~20 s at 44.1/16/2 */
#define PCM_CHUNK_BYTES         (4 * 1024)

/*
 * How long queued audio takes to stop being audible when the thing that
 * produced it has gone away.
 *
 * Pulling the card mid-track used to cut the sound at the press of the
 * ejector: the decode loop noticed the volume had gone, broke out with
 * TRACK_MEDIA_GONE, and the shared "not a clean end" path threw the ring
 * away -- up to twenty seconds of already-decoded audio, discarded at
 * memcpy speed. Nothing was wrong with those samples. They were decoded
 * before the card left and they are in PSRAM; the card is not needed to
 * play them and the click at the end of them is the only part of that
 * event the listener should not have heard.
 *
 * So the ring plays on, and this is how long it takes to get to silence.
 * The ramp is the whole point: 20 s of music ending abruptly because the
 * buffer ran out is the same click, twenty seconds later. The fade
 * finishes well before the ring empties, and whatever is left after it
 * is dropped -- silence multiplied by anything is still silence, and
 * there is no reason to clock it out.
 *
 * Three seconds, against a ring that holds twenty. The floor is far
 * lower than this -- a few hundred milliseconds is already longer than
 * a cycle of anything audible (a 20 Hz fundamental is 50 ms), so the
 * ramp cannot be heard as a pitch and the click is gone. But the click
 * is not the only thing worth avoiding: a fade short enough to be
 * over before the hand has left the card reads as a cut with the edge
 * sanded off, and the audio to do better than that is already sitting
 * in PSRAM, paid for.
 *
 * So the constraint from below is "not a click" and the constraint from
 * above is "not a player that ignores the eject", and three seconds is
 * a long way from both. It is also the number a crossfade would want --
 * an overlap this length is a musical fade rather than a debounce --
 * which is the point of picking it here rather than the smallest value
 * that removes the artefact.
 *
 * It must stay comfortably under what the ring holds, because the ramp
 * is applied to queued audio and there is no more coming. Twenty
 * seconds at 44.1 kHz is the nominal figure; a ring caught part-full
 * has less, and the writer handles a ramp that outruns its audio (see
 * the underrun branch there) rather than this number being trusted.
 *
 * PREP FOR CROSSFADE, AND THAT IS THE REASON IT LOOKS LIKE THIS.
 *
 * A crossfade is two ramps and a mix, and the hard part is not the mix
 * -- it is having somewhere to put a gain that is applied per frame, in
 * the task that owns the samples, at the moment they are handed to the
 * hardware. That is what this adds. fade_apply() below walks a chunk
 * applying an interpolated gain; a crossfade is the same walk with a
 * second ring read alongside it and the complementary gain, and the
 * ramp state (s_fade_pos, s_fade_frames) is already per-ramp rather
 * than per-track so a second one can exist next to it.
 *
 * What deliberately is NOT here yet: the mix, the second read, and the
 * overlap accounting that decides when the outgoing track's ramp starts
 * relative to the incoming one's. Those need the decode loop to start
 * the next track early on purpose, which is a scheduling change and not
 * this one.
 */
#define FADE_OUT_MS             (3000)

/*
 * Two rings, alternating, one track each.
 *
 * A track boundary is currently a drain: the decode loop waits for the
 * ring to empty before it returns, so the next track cannot start
 * decoding until the last sample of this one has been handed to I2S.
 * Nothing can be decoded ahead into a buffer that is still being
 * played, which is why there are two.
 *
 * This patch alternates them without overlapping them -- track N fills
 * ring N&1 while the writer plays it out, exactly as before, and the
 * next track fills the other one. Same behaviour, same drain, but the
 * handoff the writer needs for gapless is now what actually moves
 * playback from one track to the next, so it is exercised on every
 * boundary before anything depends on it.
 *
 * 3.5 MB each, 7 MB total, against the single 10 MB ring this replaced.
 */
#define PCM_RINGS               (2)

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

/*
 * How fast the decode loop is allowed to run while a finished track is
 * still playing out of the other ring, as a multiple of real time.
 *
 * The decode-ahead is otherwise unthrottled, and for the whole of a
 * twenty-second tail it is the only thing in the system with nothing to
 * wait for: it decodes as fast as the card and the CPU allow, writing
 * megabytes into a PSRAM ring while the next file's index build streams
 * megabytes more through PSRAM behind it. The DPI peripheral is reading
 * the framebuffer out of the same PSRAM continuously at ~105 MB/s and
 * cannot wait for anything, so it loses lines and the panel goes cyan
 * for a frame -- see the note on the pixel clock at the top of this
 * file, which has the arithmetic. Every boundary flashed.
 *
 * Nothing is gained by the burst. There is a whole tail's worth of time
 * to prepare the next track and only a second or two of audio needs to
 * exist by the time the handoff comes; the rest of the ring can fill
 * afterwards, at the rate the writer drains it. So the loop is paced
 * back to a multiple of playback speed, which turns a one-second flood
 * into a trickle spread over the tail and leaves the DPI fetch its
 * bandwidth.
 *
 * 3x rather than 1x because the point is still to be ahead: at three
 * times real time the ring is full again well before the tail ends,
 * with the traffic spread over seconds instead of concentrated into a
 * burst. Pacing applies ONLY while a tail is playing. Once this track is
 * the audible one it is the only decoder running and it runs flat out,
 * which is what a card stall needs.
 */
#define TAIL_DECODE_SPEEDUP     (3)

/*
 * The same throttle, applied to the other burst: a ring refilling from
 * empty after a seek.
 *
 * xStreamBufferReset() in the seek path drops everything queued, which
 * is the right thing to do -- without it the old position plays on for
 * most of a second after the jump. What it leaves behind is a 3.5 MB
 * ring with nothing in it and a decode loop with nothing to wait for,
 * so the loop runs flat out writing PCM into PSRAM until the ring is
 * full again. TAIL_DECODE_SPEEDUP does not cover this: s_tail_pending
 * is false, because there is no outgoing track playing out.
 *
 * That burst is what the cyan flash follows. It arrives on the release
 * of a drag rather than during it -- the drag only records a target,
 * and the decode loop is where the seek is serviced -- and it happens
 * with playback paused, which is when it is worst: nothing drains the
 * ring, so the refill runs the full twenty seconds in one go.
 *
 * Paced to the same 3x, and only until the ring holds enough to survive
 * a card stall. Past that mark the loop goes back to running flat out,
 * because a deep ring is the whole defence against a slow device and
 * filling it is not urgent once the shallow part is there.
 */
#define REFILL_DECODE_SPEEDUP   (3)

/*
 * How full is enough to stop pacing a post-seek refill, in percent.
 *
 * Low enough that the burst is over quickly and high enough to cover the
 * worst hold storage_io.h admits to. 25% of the ring is ~5 s, against a
 * 16 KB chunk on a healthy card at ~2 ms.
 */
#define REFILL_PACE_UNTIL_PCT   (25)

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
/*
 * The first write of the boot, retried.
 *
 * A panic anywhere in the program resets the P4 without releasing the
 * bus, and a slave interrupted mid-byte can sit on SDA into the next
 * boot. The first transmit then NACKs -- which the v5 I2C master driver
 * reports as ESP_ERR_INVALID_STATE -- and since app_main() checks this
 * function, the recovery boot aborts before the panel is up. One crash
 * becomes an unrecoverable loop with nothing on screen to say why.
 *
 * Three attempts 20 ms apart. A device that is genuinely absent still
 * fails, 60 ms later; a bus that just needs a moment gets it.
 */
static esp_err_t reg_write_retry(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < 3; i++) {
        err = reg_write(dev, reg, val);
        if (err == ESP_OK) {
            if (i) ESP_LOGW(TAG, "expander answered on attempt %d", i + 1);
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return err;
}

static esp_err_t io_expanders_init(void)
{

    ESP_RETURN_ON_ERROR(add_dev(PI4IOE_ADDR_1, &s_exp1), TAG, "expander 0x43 absent");
    ESP_RETURN_ON_ERROR(reg_write_retry(s_exp1, PI4IOE_REG_CHIP_RESET, 0xFF), TAG, "reset 1");
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

/*
 * The ring the decoder is filling. An alias for s_ring[s_ring_fill],
 * kept because everything that queues audio wants it and rewriting all
 * of that to index would say nothing extra.
 *
 * NULL means no ring at all, which is a boot where the PSRAM could not
 * be had. play_file() refuses tracks in that state.
 */
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
 *
 * The ring no longer goes away -- it is created once in app_main() and
 * outlives every track -- so the window that crash came through is
 * gone. The published int stays anyway: it is what media_task's
 * headroom checks are written against, and handing out the handle now
 * would be re-earning the right to a use-after-free the first time
 * anything else acquires a lifetime.
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
 *
 * PER RING, not per player, and that is the whole of the twenty-second
 * bug this replaced.
 *
 * With two rings there are two tracks in flight: the writer is playing
 * out the tail of the old one while the decode loop is a whole ring
 * ahead into the new one. One set of counters cannot describe both, so
 * the new track's frame count landed on top of the old track's while
 * the old track was still the audible one -- and the gate then zeroed
 * it again at the handoff, against a ring that already held twenty
 * seconds of decoded audio. decoded-minus-queued went negative, clamped
 * to zero, and stayed there until the decoder had produced a ring's
 * worth again: the position sat still for exactly as long as the ring
 * is deep.
 *
 * Indexed by ring, they describe the track that ring holds. The writer
 * reads the slot for the ring it is draining, which is by definition
 * the track being heard, and the answer is right on both sides of a
 * handoff without anything being reset at it. */
static volatile uint64_t s_frames_out[PCM_RINGS];
static volatile uint32_t s_frames_rate[PCM_RINGS];

/*
 * Bytes per frame in the ring, which is not bytes per frame in the file.
 *
 * The I2S slots are stereo 16-bit and mono is duplicated into both on
 * the way in, so whatever the file is, what is queued is four bytes a
 * frame. The channel count that used to divide this was the FILE's, so
 * a mono track had its queued audio counted as twice as many frames as
 * it held and its position ran backwards from the moment the ring had
 * anything in it. Nothing in the ring is ever mono; there is no channel
 * count to consult.
 */
#define PCM_BYTES_PER_FRAME     (4)

/*
 * The ring's memory, allocated once and never freed.
 *
 * Separate from the handle because the handle is per track and this is
 * not. xStreamBufferCreateStatic() wants storage one byte larger than
 * the usable size -- the implementation keeps head and tail distinct
 * that way -- and it is the caller's job to know that.
 */
static StaticStreamBuffer_t s_pcm_struct[PCM_RINGS];
static uint8_t             *s_pcm_store[PCM_RINGS];
static StreamBufferHandle_t s_ring[PCM_RINGS];

/*
 * Which ring the decoder is filling and which one the writer is
 * playing. Equal for all of one track; they differ from the moment a
 * new track starts filling until the old one has been played out.
 *
 * Two plain ints rather than handles passed between tasks, for the same
 * reason s_ring_pct is an int: an index cannot dangle, and the writer
 * and the decode loop are on different cores.
 *
 * The decode loop owns s_ring_fill and only ever moves it to a ring the
 * writer is not on. The writer owns s_ring_play and only ever moves it
 * to s_ring_fill, and only once its own ring has run dry. Neither ever
 * moves the other's, so there is nothing to lock.
 */
static volatile int s_ring_fill;
static volatile int s_ring_play;

/*
 * Is a finished track still being played out of its ring?
 *
 * Latched, not sampled, and that is the whole of the difference between
 * this and the two versions of it that did not work.
 *
 * The first asked `s_ring_play != s_ring_fill`. The decode loop does not
 * move s_ring_fill when a track starts -- it moves it when the FIRST
 * BLOCK is decoded, one decoder_open() later, which on a Xing-less MP3
 * is two seconds of index build. Everything in track_change_begin(),
 * which runs before the open, therefore saw the indices still equal.
 *
 * The second asked whether s_ring[s_ring_play] was empty, at the moment
 * it was asked, from the decode-loop task. That is a question about two
 * volatile words and a stream buffer that another task is draining, read
 * during the window between one track's decode ending and the next one's
 * first block -- the one window in which s_ring_play, s_ring_fill and
 * s_pcm do not agree about anything. It answered "empty" against a ring
 * with twenty seconds in it.
 *
 * So it is neither computed nor sampled now: it is SET at the one moment
 * the answer is unambiguous -- the end of a track's decode loop, which
 * knows exactly which ring it filled and exactly how much it left there
 * -- and CLEARED by the writer, which is the task that empties that ring
 * and therefore the only one that can say when the last sample has gone.
 * Those two events are the two the screen is allowed to change on: a
 * press, which clears it, and the ring running out, which clears it.
 */
static volatile bool s_tail_pending;
static volatile int  s_tail_ring;

/*
 * Set when a seek empties the ring, cleared when it has refilled past
 * REFILL_PACE_UNTIL_PCT. Read only by the decode loop, which is also
 * the only writer, so it needs no more protection than this.
 */
static volatile bool s_refill_pacing;

static bool tail_playing(void)
{
    return s_tail_pending;
}

/*
 * Set by play_file() before it drains the ring, and the reason the
 * pause gate below is not simply "if (!s_playing)".
 *
 * The end of a finished track spins waiting for the ring to empty,
 * which is waiting on the writer to move.
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
/*
 * A seek asks the writer to throw away what is queued.
 *
 * Set by the decode loop at a seek commit, cleared by the writer when
 * the ring is empty. The handshake is one word each way because the two
 * tasks already communicate that way -- see s_ring_pct and
 * s_tail_pending -- and because anything richer would need a lock on the
 * path this exists to keep lock-free.
 *
 * WHY NOT xStreamBufferReset(), WHICH THIS REPLACES
 *
 * Because it is the cyan flash. Nineteen seeks with the reset skipped
 * (SEEK_KEEP_RING, 0507) produced no flash at all; the same build
 * flashed at the next track boundary, where a ring is reset by another
 * path. 0500 had already excluded the refill that follows the reset, so
 * the two bracket it.
 *
 * The mechanism fits even if the last link to a DSI frame is not proven:
 * xStreamBufferReset() rewrites a stream buffer's head and tail while
 * i2s_writer_task may be parked inside xStreamBufferReceive() on that
 * same buffer. FreeRTOS says a reset fails if a task is blocked on
 * either side, and a writer released against indices that moved beneath
 * it is not a defined state to be in at priority 6.
 *
 * WHY NOT DRAIN FROM THE DECODE LOOP
 *
 * That would be two readers on a structure that supports one. The
 * discard has to happen in the task that already owns the read, which
 * is this one.
 *
 * WHAT THIS IS NOT
 *
 * Not the track-boundary drain that used to wait for the ring to play
 * out; that blocked the next track for the length of a ring and was
 * removed for gapless (see the note above PCM_RING_BYTES). This throws
 * the samples away at memcpy speed and waits for nobody.
 */
static volatile bool s_pcm_flush;

/*
 * Why the flush was asked for, for the log line only.
 *
 * The drop used to be the seek path's and said so. It has a second
 * caller now -- the end of a fade -- and "seek: dropped 3200 KB" during
 * a card removal is a line that sends you looking at the wrong path.
 *
 * A const char * written by one task and read by another with no
 * ordering against the flag itself. That is fine for what it is: the
 * pointers are all string literals with static lifetime, so the worst a
 * race can do is attribute one drop to the wrong reason. It is a label,
 * not a decision.
 */
static const char * volatile s_flush_why = "seek";

/*
 * The fade-out ramp.
 *
 * Requested by the decode loop, run by the writer, and owned by the
 * writer for as long as it is running -- s_fade_pos is only ever
 * touched inside fade_apply(), which only the writer calls. The request
 * side sets the length first and the flag last, so the writer cannot
 * see s_fade_out true against a zero length.
 *
 * Not a mutex and not a queue for the same reason as everything else on
 * this path: the writer must not block on anything but DMA.
 */
static volatile bool     s_fade_out;
static volatile uint32_t s_fade_frames;   /* length of the ramp, in frames */
static volatile uint32_t s_fade_pos;      /* how far through it we are */

/*
 * The ramp has finished and the screen is describing nothing.
 *
 * Set by the writer at the end of a fade -- the task that consumed the
 * last audible sample, and therefore the only one that knows when the
 * track stopped being heard. This is the same argument as s_tail_pending
 * and it is deliberately the same shape: latched at the one unambiguous
 * moment, cleared by the task that acts on it.
 *
 * Consumed by player_loop(), which is where load_track_visuals() and
 * ui_clear_art() already run. The writer must not draw -- it is at
 * priority 6 with a DMA deadline, and there is exactly one writer to the
 * framebuffer in this program and it is not this task.
 */
static volatile bool     s_fade_cleanup;

/*
 * THE RATE-CHANGE DIP.
 *
 * A crossfade cannot span a sample-rate change -- there is no
 * resampler, mixing 44.1 into 48 is a nine percent pitch shift for the
 * length of the overlap, and reconfiguring the I2S clock disables the
 * channel anyway. 0719 made that refusal visible; this makes it sound
 * like something other than a cut.
 *
 * Not an overlap: a ramp DOWN over half the configured crossfade,
 * ending exactly as the outgoing ring does, then the drain and the
 * reconfigure, then a ramp UP over the other half. The two never
 * coexist, so nothing is ever mixed and no rate is ever wrong. The
 * total is the crossfade the listener asked for, spent in two halves
 * instead of one overlap.
 *
 * One direction at a time, so one position and one length. `arm` is the
 * ring occupancy at which the down ramp should start -- the same
 * countdown xfade_can_start() uses, for the same reason: the outgoing
 * ring can only shrink once the decode loop has moved on, so it is a
 * countdown with a guaranteed direction rather than a race.
 */
static volatile int32_t  s_dip_dir;       /* -1 down, 0 none, +1 up */
static volatile uint32_t s_dip_frames;
static volatile uint32_t s_dip_pos;
static volatile uint32_t s_dip_arm;       /* frames; 0 = not armed */
/* The up half, held between the down ramp and the reconfigure that has
 * to happen before it can be measured in the new rate's frames. */
static volatile uint32_t s_dip_in_frames;

/* Gain for frame `pos` of a ramp of `frames`, Q15. Linear, because over
 * a second or two of a dip to actual silence there is no second signal
 * for an equal-power curve to preserve power against. */
static inline int32_t dip_gain_q15(uint32_t pos, uint32_t frames, bool up)
{
    if (!frames) return 32767;
    if (pos >= frames) return up ? 32767 : 0;
    const uint32_t t = (uint32_t)(((uint64_t)pos << 15) / frames);
    return (int32_t)(up ? t : (32768u - t));
}

/* Apply the running dip to a chunk, advancing it. Writer only. */
static void dip_apply(int16_t *pcm, size_t frames)
{
    const bool up = (s_dip_dir > 0);
    for (size_t i = 0; i < frames; i++) {
        const int32_t g = dip_gain_q15(s_dip_pos + (uint32_t)i,
                                       s_dip_frames, up);
        pcm[2 * i]     = (int16_t)(((int32_t)pcm[2 * i]     * g) >> 15);
        pcm[2 * i + 1] = (int16_t)(((int32_t)pcm[2 * i + 1] * g) >> 15);
    }
    s_dip_pos += (uint32_t)frames;
}

/* Is anything still going to come out of the speaker because of a fade?
 * Read by the UI task's idle gate, which would otherwise cut the
 * amplifier out from under the ramp -- nothing is decoding during one,
 * which is exactly the condition that gate uses to decide it is over. */
static bool fade_out_active(void)
{
    return s_fade_out;
}

/*
 * Start a ramp to silence over whatever is queued.
 *
 * rate is the rate the samples will be CLOCKED OUT at, not the rate of
 * the file: the ring holds what the hardware is about to play, and a
 * ramp measured in frames only lands on FADE_OUT_MS if it is measured
 * against the clock that consumes them.
 */
static void fade_out_begin(uint32_t rate)
{
    if (s_fade_out) return;                 /* one ramp is enough */
    /* Before the first track, or if the sink never got a format. A ramp
     * of the wrong length is still a ramp; not fading is a click. */
    if (rate == 0) rate = 44100;

    uint32_t frames = (uint32_t)((uint64_t)rate * FADE_OUT_MS / 1000u);
    if (frames == 0) frames = 1;            /* no division by zero below */

    s_fade_pos    = 0;
    s_fade_frames = frames;
    s_fade_out    = true;                   /* last: see above */
}

/*
 * A new track has arrived while a ramp was running.
 *
 * The ramp belongs to audio the listener has already been told is over,
 * and leaving it running would apply the tail of it to the first
 * moments of whatever is starting -- a track that begins half faded and
 * then, worse, hits the end of the ramp and drops to zero.
 *
 * The queued audio goes with it, and that is not the gapless path
 * losing its tail: this only fires when a fade was actually running,
 * and a fade only runs when the queued audio has already been declared
 * unwanted. A clean track boundary never gets here.
 */
static void fade_out_cancel(void)
{
    if (!s_fade_out) return;
    s_fade_out  = false;
    s_flush_why = "fade cancelled";
    s_pcm_flush = true;
    /*
     * And the screen is not cleared, because there is now something for
     * it to describe. The clear belongs to a fade that reached silence
     * with nothing behind it; this one was overtaken by a track. Left
     * set, it would blank the incoming track's title and cover a moment
     * after they were drawn.
     */
    s_fade_cleanup = false;
}

/*
 * Apply the ramp to one chunk, in place.
 *
 * Interleaved stereo 16-bit, which is what the ring holds and the only
 * thing it ever holds (see PCM_BYTES_PER_FRAME). Linear in amplitude,
 * Q15, so the gain is an integer multiply and a shift per sample rather
 * than a float per sample on a path with a DMA deadline.
 *
 * Linear rather than the equal-power curve a crossfade wants, because
 * this is a fade against silence and not against another signal: there
 * is nothing for the power to be equal to. The curve is one expression
 * on the line below when that changes.
 *
 * Past the end of the ramp the gain is zero rather than the loop
 * stopping. The caller drops the rest of the ring immediately after, so
 * this only covers the frames between the ramp ending and the chunk
 * boundary -- but they are frames, and they would otherwise be the
 * click this exists to remove.
 */
/* ------------------------------------------------------------------ */
/* Crossfade                                                           */
/* ------------------------------------------------------------------ */

/*
 * Equal power, 33 points of a quarter cosine, Q15.
 *
 * NOT the linear ramp fade_apply() uses, and the difference is the whole
 * reason this is a table rather than a subtraction.
 *
 * A fade to silence is linear because there is nothing on the other side
 * of it: halfway through, half the amplitude is exactly what half a fade
 * should sound like. A crossfade has a second signal there, and two
 * uncorrelated signals at 0.5 amplitude sum to 0.707 of the power, not
 * 1.0 -- a ~3 dB hole in the middle of every transition. It is the most
 * common complaint about naive crossfaders and it is audible as a lull
 * exactly where the listener is paying attention.
 *
 * cos and sin instead: cos^2 + sin^2 = 1, so the power is constant for
 * uncorrelated material. The table is the cosine and the sine is the
 * same table read backwards, which is why there is only one.
 *
 * Equal power is wrong for CORRELATED material -- the same track against
 * itself, or a track against its own reverb tail, where amplitudes add
 * rather than powers, and linear would be right. Nothing can be right
 * for both, every player picks one, and music at a track boundary is
 * overwhelmingly the uncorrelated case.
 *
 * 33 points with linear interpolation between them: over a 3 s ramp each
 * segment is 94 ms, and the curve's second derivative is small enough
 * that the error never exceeds a tenth of a dB.
 */
static const uint16_t k_eqpower[33] = {
    32767, 32728, 32609, 32412, 32137, 31785, 31356, 30852, 30273, 29621, 28898, 28105, 27245, 26319, 25329, 24279, 23170, 22005, 20787, 19519, 18204, 16846, 15446, 14010, 12539, 11039, 9512, 7962, 6393, 4808, 3212, 1608, 0
};

/* Gain at t, t in Q16 over [0,65536]. Outgoing when `out` is true. */
static int32_t eqpower_q15(uint32_t t, bool out)
{
    if (t > 65536u) t = 65536u;
    if (!out) t = 65536u - t;           /* sin(x) == cos(90-x) */

    const uint32_t idx = t >> 11;               /* 0..32 */
    const uint32_t frac = t & 0x7FFu;           /* 0..2047 */
    if (idx >= 32) return k_eqpower[32];

    const int32_t a = k_eqpower[idx];
    const int32_t b = k_eqpower[idx + 1];
    return a + (((b - a) * (int32_t)frac) >> 11);
}

/*
 * Per-ring level trim, applied only during an overlap.
 *
 * WHY THIS EXISTS EVEN THOUGH REPLAYGAIN DOES THE SAME JOB.
 *
 * When ReplayGain is on and both tracks have been measured before, it
 * does nothing: rg_scale was applied to the samples as they were
 * decoded, both rings are at the reference, and the levels published
 * for them say so -- the difference is zero and the trims stay unity.
 *
 * It still earns its place in that mode, because "both measured" is not
 * guaranteed. The first play of a track measures at unity and applies
 * nothing, so a never-heard track crossfading against a levelled one is
 * a real mismatch with ReplayGain fully on. The published figure is the
 * EFFECTIVE level of what went into the ring, which makes that case fall
 * out of the same arithmetic instead of needing its own.
 *
 * When ReplayGain is OFF the two tracks are at whatever level they were
 * mastered at, and that difference is inaudible at a hard cut and very
 * audible during three seconds of overlap: a quiet track fading in under
 * a loud one does not sound like a crossfade, it sounds like the next
 * song failing to start. So the overlap gets its own match, for its own
 * duration, and the moment it ends both tracks are back at their own
 * levels -- the listener asked for ReplayGain off and does not get it
 * imposed by the back door.
 *
 * ATTENUATION ONLY. The louder track is brought down to the quieter one
 * and never the reverse. Boosting risks clipping, and clipping on a
 * transition is a worse artefact than the imbalance it would fix.
 *
 * The numbers come from the loudness section of the sidecar, which is
 * measured on every play since 0612 whether or not ReplayGain is
 * applying anything -- that patch is what makes this possible with the
 * switch off. The waveform's levels would also give a proxy, but a peak
 * envelope is not loudness: a sparse track with loud transients and a
 * dense quiet one can share a peak envelope and be 10 dB apart. The
 * LUFS figure is the measurement that was actually taken.
 *
 * Unknown on either side means unity on both. A guess about the
 * relationship between two tracks is worse than leaving them alone.
 */
static volatile float s_ring_lufs[PCM_RINGS];
static volatile bool  s_ring_lufs_known[PCM_RINGS];

/* Never trim by more than this. A 12 dB gap between two tracks is a
 * mastering difference the listener can hear across the whole song, not
 * something a three-second overlap should hide -- and pulling one track
 * down 12 dB to meet the other makes the transition itself the loudest
 * thing about it. */
#define XFADE_MATCH_MAX_DB      (6.0f)

/*
 * The overlap, as the writer sees it.
 *
 * s_xfade_armed is set by the decode loop when it starts a track that
 * MAY be crossfaded into -- the previous track ended on its own, the
 * length is not zero, and the same-album rule allows it. The writer
 * decides whether one actually happens, because the rest of the
 * conditions are about the rings and only the writer can see them:
 * matching rates, matching channel counts, and enough audio queued on
 * both sides.
 */
static volatile bool     s_xfade_armed;
static volatile uint32_t s_xfade_ms;

static bool     s_xfade_active;     /* writer-only from here down */
static uint32_t s_xfade_pos;        /* frames into the overlap */
static uint32_t s_xfade_frames;     /* its length */
static int32_t  s_xfade_gain_a;     /* match trims, Q15 */
static int32_t  s_xfade_gain_b;

static void fade_apply(int16_t *pcm, size_t frames)
{
    const uint32_t total = s_fade_frames;
    uint32_t pos = s_fade_pos;

    for (size_t f = 0; f < frames; f++) {
        const int32_t g = (pos >= total)
            ? 0
            : (int32_t)(32768u - (uint32_t)(((uint64_t)32768u * pos) / total));

        pcm[2 * f + 0] = (int16_t)(((int32_t)pcm[2 * f + 0] * g) >> 15);
        pcm[2 * f + 1] = (int16_t)(((int32_t)pcm[2 * f + 1] * g) >> 15);

        if (pos < total) pos++;
    }

    s_fade_pos = pos;
}

/*
 * Decide whether an overlap can start right now.
 *
 * Every condition here is about the rings, which is why it lives on the
 * writer rather than in the decode loop with the rest of the policy.
 */
static bool xfade_can_start(int play, int fill, size_t avail_a, size_t avail_b,
                            uint32_t need_frames)
{
    if (!s_xfade_armed || s_xfade_active || s_fade_out) return false;
    if (play == fill) return false;                 /* no second track yet */

    const uint32_t rate = s_frames_rate[play];
    /*
     * Rates must match, and there is no resampler to make them.
     *
     * 44.1 into 48 is not rare on a mixed library, and mixing them by
     * pretending is a pitch shift on one of the two tracks for the
     * length of the overlap. audio_out is reconfigured at the boundary
     * for exactly this reason; a crossfade cannot span that.
     */
    if (rate == 0 || s_frames_rate[fill] != rate) return false;

    /*
     * THE TRIGGER IS THE OUTGOING RING RUNNING DOWN, NOT FILLING UP.
     *
     * 0613 had this backwards. It asked for at least need_bytes queued
     * -- reading "enough left to fade out of" as a minimum -- and with
     * decode-ahead the outgoing ring holds twenty seconds at the moment
     * the incoming track produces its first block. The minimum was
     * satisfied instantly, so the overlap began at the START of the tail
     * instead of at its end: three seconds of crossfade, then seventeen
     * seconds of the outgoing track dropped unheard.
     *
     * The right question is when the outgoing ring has come DOWN to the
     * overlap length. play != fill above means the decode loop has moved
     * on, so this ring can only shrink from here -- the test is a
     * countdown with a guaranteed direction, not a race.
     *
     * One chunk of slack because avail_a falls in chunk-sized steps and
     * an exact comparison would step straight past the mark. Being 23 ms
     * early is inaudible; missing entirely is a hard cut.
     */
    const size_t need_bytes = (size_t)need_frames * PCM_BYTES_PER_FRAME;
    if (avail_a > need_bytes + PCM_CHUNK_BYTES) return false;

    /*
     * And enough of the incoming decoded to fade into, or the outgoing
     * one ramps down into an empty ring. Four chunks rather than the
     * whole overlap: requiring the incoming ring to hold the full length
     * would mean waiting on a decode while the outgoing ring drains past
     * the point where a crossfade is still possible, which turns a slow
     * open into a hard cut instead of a shorter fade.
     */
    if (avail_b < PCM_CHUNK_BYTES * 4) return false;

    return true;
}

/*
 * The overlap that will actually fit.
 *
 * The configured length, or whatever is left of the outgoing track if
 * that is less. A tail shorter than the setting is the normal case at
 * the end of a playlist, after a seek near the end, or when the incoming
 * track's open ran long -- and a short crossfade is a better answer than
 * none, because the alternative is the ramp running past the end of the
 * audio it is ramping.
 *
 * Below XFADE_MIN_MS there is nothing worth doing: a quarter-second
 * overlap is not a crossfade, it is a hard cut with a smear on it, and
 * the plain handoff is cleaner.
 */
#define XFADE_MIN_MS    (400)

static uint32_t xfade_fit(uint32_t want_frames, size_t avail_a, uint32_t rate)
{
    const uint32_t have = (uint32_t)(avail_a / PCM_BYTES_PER_FRAME);
    if (have < want_frames) want_frames = have;

    const uint32_t floor_frames = (uint32_t)(((uint64_t)rate * XFADE_MIN_MS) / 1000u);
    return want_frames < floor_frames ? 0 : want_frames;
}

/*
 * The level match, computed once when the overlap starts.
 *
 * See s_ring_lufs. Unity on both sides whenever ReplayGain is applying,
 * when either measurement is missing, or when the two are already close
 * enough that the arithmetic would be noise.
 */
static void xfade_match(int play, int fill)
{
    s_xfade_gain_a = 32768;
    s_xfade_gain_b = 32768;

    if (!s_ring_lufs_known[play] || !s_ring_lufs_known[fill]) return;

    float diff = s_ring_lufs[fill] - s_ring_lufs[play];   /* + means B louder */
    if (diff > -0.5f && diff < 0.5f) return;

    if (diff >  XFADE_MATCH_MAX_DB) diff =  XFADE_MATCH_MAX_DB;
    if (diff < -XFADE_MATCH_MAX_DB) diff = -XFADE_MATCH_MAX_DB;

    /* Attenuation only: whichever is louder comes down to the other. */
    const float atten = powf(10.0f, -(diff < 0 ? -diff : diff) / 20.0f);
    const int32_t q = (int32_t)(atten * 32768.0f + 0.5f);
    if (diff > 0) s_xfade_gain_b = q;
    else          s_xfade_gain_a = q;
}

/*
 * Mix `frames` of B into A in place, ramping across the chunk.
 *
 * The gains are computed at both ends of the chunk and interpolated per
 * frame between them, rather than evaluated per frame from the table.
 * A chunk is 1024 frames -- 23 ms -- and the curve is smooth enough over
 * that span that the straight line between its endpoints is inaudible,
 * where a per-frame table lookup would be two extra multiplies on the
 * one task in this program with a DMA deadline.
 */
static void xfade_mix(int16_t *a, const int16_t *b, size_t frames)
{
    const uint32_t total = s_xfade_frames;
    const uint32_t p0 = s_xfade_pos;
    const uint32_t p1 = (p0 + frames > total) ? total : p0 + frames;

    const uint32_t t0 = (uint32_t)(((uint64_t)p0 << 16) / total);
    const uint32_t t1 = (uint32_t)(((uint64_t)p1 << 16) / total);

    /* Match trim folded in here, so the mix loop stays two multiplies
     * and an add per sample. */
    const int32_t ga0 = (eqpower_q15(t0, true)  * s_xfade_gain_a) >> 15;
    const int32_t ga1 = (eqpower_q15(t1, true)  * s_xfade_gain_a) >> 15;
    const int32_t gb0 = (eqpower_q15(t0, false) * s_xfade_gain_b) >> 15;
    const int32_t gb1 = (eqpower_q15(t1, false) * s_xfade_gain_b) >> 15;

    for (size_t f = 0; f < frames; f++) {
        const int32_t step = frames > 1 ? (int32_t)((f << 15) / frames) : 0;
        const int32_t ga = ga0 + (((ga1 - ga0) * step) >> 15);
        const int32_t gb = gb0 + (((gb1 - gb0) * step) >> 15);

        for (int c = 0; c < 2; c++) {
            const size_t i = 2 * f + (size_t)c;
            /*
             * Saturated, not wrapped. Equal power keeps the SUM of
             * powers at unity, which is not the same as keeping the sum
             * of amplitudes there: two loud, correlated frames at the
             * midpoint reach 1.41 full scale. Rare, brief, and a clamp
             * is a far better answer to it than a sign flip.
             */
            int32_t v = (((int32_t)a[i] * ga) >> 15) +
                        (((int32_t)b[i] * gb) >> 15);
            if (v >  32767) v =  32767;
            if (v < -32768) v = -32768;
            a[i] = (int16_t)v;
        }
    }

    s_xfade_pos = p1;
}

static volatile bool s_playing;

static void ring_publish(void);

/* Drains the ring into I2S and nothing else, so the only thing it ever
 * blocks on is DMA. */
static void i2s_writer_task(void *arg)
{
    uint8_t *buf = heap_caps_malloc(PCM_CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    /* The incoming track's chunk during an overlap. Internal rather than
     * DMA-capable: it is only ever read by the mix, never handed to the
     * hardware. One chunk, allocated for the life of the task, because
     * allocating it at the start of a crossfade would put a malloc on
     * the path with the DMA deadline. */
    uint8_t *mixbuf = heap_caps_malloc(PCM_CHUNK_BYTES, MALLOC_CAP_8BIT);
    /* No ring to drain into I2S without this, and nothing else can
     * recover it, so the task ends. play_file() sees a NULL s_pcm and
     * refuses the track rather than queueing into a ring nobody reads. */
    if (!buf) {
        ESP_LOGE(TAG, "no writer buffer; audio output is dead this boot");
        s_pcm = NULL;
        vTaskDelete(NULL);
        return;
    }
    /* Playback survives without the mix buffer; crossfading does not.
     * Degrading to hard cuts is the right failure -- the alternative is
     * refusing to play music because a nicety could not be allocated. */
    if (!mixbuf) {
        ESP_LOGW(TAG, "no mix buffer; crossfade disabled this boot");
    }

    while (1) {
        /*
         * The seek discard. See s_pcm_flush.
         *
         * Ahead of the pause gate, deliberately: a seek while paused is
         * still a seek, and leaving the old position queued until resume
         * is the artefact this whole path exists to avoid.
         *
         * Receives with a zero timeout until the ring reports empty, so
         * it never blocks and cannot be the reason the writer misses a
         * DMA deadline. Both rings, because a seek during a decode-ahead
         * has a tail in one and the new position filling the other.
         */
        if (s_pcm_flush) {
            size_t dropped = 0, got;
            for (int r = 0; r < PCM_RINGS; r++) {
                if (!s_ring[r]) continue;
                while ((got = xStreamBufferReceive(s_ring[r], buf,
                                                   PCM_CHUNK_BYTES, 0)) > 0) {
                    dropped += got;
                }
            }
            s_pcm_flush = false;
            ring_publish();
            ESP_LOGI(TAG, "%s: dropped %u KB of queued audio",
                     s_flush_why, (unsigned)(dropped / 1024));
        }

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

        /*
         * The last sample of the finished track has gone to I2S.
         *
         * This is the event the screen has been waiting for, and it is
         * detected here because this is the task that consumed it. Note
         * that it is NOT the handoff below: the handoff needs the decode
         * loop to have produced a first block for the next track, which
         * on a long index build is seconds later, and the truthful
         * moment to stop showing a track is when it stops being heard --
         * not when its successor is ready to be.
         *
         * Checked before the pause gate would matter and on every pass,
         * including the ones where the receive below times out, because
         * a ring that has run dry generates no other event.
         */
        if (s_tail_pending && xStreamBufferIsEmpty(s_ring[s_tail_ring])) {
            s_tail_pending = false;
            ESP_LOGI(TAG, "the finished track has played out; "
                          "the screen is the next track's now");
        }

        /*
         * The handoff.
         *
         * The decoder has moved on to the other ring and this one has
         * nothing left in it, so everything of the old track that will
         * ever be heard has been. Moving to the ring being filled is
         * what makes the next track audible, and doing it here -- in
         * the task that owns playback -- is what makes it happen at the
         * right sample rather than at the right moment.
         */
        const int play = s_ring_play;
        if (play != s_ring_fill && xStreamBufferIsEmpty(s_ring[play])) {
            s_ring_play = s_ring_fill;
            /* An overlap cannot outlive the ring it was fading out of.
             * Reaching here mid-crossfade means the outgoing track ran
             * dry early -- the incoming one is now simply the track. */
            if (s_xfade_active) {
                s_xfade_active = false;
                s_xfade_armed = false;
                ESP_LOGW(TAG, "crossfade cut short: the outgoing ring emptied");
            }
            continue;
        }

        /* --- the overlap ---------------------------------------------
         *
         * Placed before the ordinary receive rather than inside it,
         * because a mixed chunk has to be dequeued from BOTH rings in
         * step and the single-ring path has no way to express that.
         *
         * Availability is checked before either receive. StreamBuffers
         * cannot be peeked or pushed back, so taking from one ring and
         * then discovering the other is short would leave frames
         * dequeued with nowhere to go. Asking first means every byte
         * taken is a byte that gets mixed.
         */
        const int fill = s_ring_fill;
        if (mixbuf && (s_xfade_armed || s_xfade_active)) {
            const size_t avail_a = xStreamBufferBytesAvailable(s_ring[play]);
            const size_t avail_b = (fill != play)
                                 ? xStreamBufferBytesAvailable(s_ring[fill]) : 0;

            if (!s_xfade_active) {
                const uint32_t rate = s_frames_rate[play];
                const uint32_t want = rate
                    ? (uint32_t)(((uint64_t)rate * s_xfade_ms) / 1000u) : 0;
                if (want && xfade_can_start(play, fill, avail_a, avail_b, want)) {
                    const uint32_t fit = xfade_fit(want, avail_a, rate);
                    if (fit == 0) {
                        /* Too little left to be worth it. Disarm rather
                         * than keep testing: the ring only shrinks from
                         * here, so this cannot become true again. */
                        s_xfade_armed = false;
                        ESP_LOGI(TAG, "no crossfade: only %u ms of the "
                                      "outgoing track left",
                                 (unsigned)((avail_a / PCM_BYTES_PER_FRAME)
                                            * 1000u / (rate ? rate : 44100)));
                    } else {
                        s_xfade_active = true;
                        s_xfade_armed  = false;
                        s_xfade_pos    = 0;
                        s_xfade_frames = fit;
                        xfade_match(play, fill);
                        ESP_LOGI(TAG, "crossfade: %" PRIu32 " ms"
                                      "%s, trim out %d%% in %d%%",
                                 fit * 1000u / rate,
                                 fit < want ? " (tail was shorter)" : "",
                                 (int)((s_xfade_gain_a * 100) / 32768),
                                 (int)((s_xfade_gain_b * 100) / 32768));
                    }
                }
            }

            if (s_xfade_active) {
                size_t n = PCM_CHUNK_BYTES;
                if (avail_a < n) n = avail_a;
                if (avail_b < n) n = avail_b;
                n &= ~(size_t)(PCM_BYTES_PER_FRAME - 1);

                if (n) {
                    xStreamBufferReceive(s_ring[play], buf, n, 0);
                    xStreamBufferReceive(s_ring[fill], mixbuf, n, 0);
                    xfade_mix((int16_t *)buf, (const int16_t *)mixbuf,
                              n / PCM_BYTES_PER_FRAME);
                    audio_out_write(buf, n);
                    ring_publish();

                    if (s_xfade_pos >= s_xfade_frames) {
                        /*
                         * Done. What is left of the outgoing ring is the
                         * end of a track that has finished being heard,
                         * at a gain of zero -- so it is dropped and the
                         * writer moves to the incoming ring, which is
                         * exactly the ordinary handoff arriving early.
                         */
                        s_xfade_active = false;
                        const size_t left =
                            xStreamBufferBytesAvailable(s_ring[play]);
                        xStreamBufferReset(s_ring[play]);
                        s_ring_play = fill;
                        ring_publish();
                        ESP_LOGI(TAG, "crossfade done; dropped %u KB of the "
                                      "outgoing track", (unsigned)(left / 1024));
                    }
                    continue;
                }

                /*
                 * One side had nothing this pass -- in practice the
                 * incoming ring, starved behind a decode that has only
                 * just started.
                 *
                 * The outgoing track keeps playing, but AT ITS CURRENT
                 * RAMP GAIN and with the ramp held where it is. Falling
                 * through to the ordinary single-ring path instead would
                 * write it at unity, which partway down a fade is a step
                 * back up in level -- the artefact the crossfade exists
                 * to remove, produced by the crossfade.
                 *
                 * Holding the position rather than advancing it means a
                 * starved overlap stretches rather than losing its
                 * incoming half. It resumes where it left off when the
                 * decode catches up, and if the outgoing ring empties
                 * first the handoff at the top of the loop ends it.
                 */
                const size_t solo = (avail_a < PCM_CHUNK_BYTES)
                                  ? (avail_a & ~(size_t)(PCM_BYTES_PER_FRAME - 1))
                                  : PCM_CHUNK_BYTES;
                if (solo) {
                    xStreamBufferReceive(s_ring[play], buf, solo, 0);

                    const uint32_t t = (uint32_t)
                        (((uint64_t)s_xfade_pos << 16) / s_xfade_frames);
                    const int32_t g =
                        (eqpower_q15(t, true) * s_xfade_gain_a) >> 15;

                    int16_t *pcm = (int16_t *)buf;
                    const size_t n_s = (solo / PCM_BYTES_PER_FRAME) * 2;
                    for (size_t i = 0; i < n_s; i++) {
                        pcm[i] = (int16_t)(((int32_t)pcm[i] * g) >> 15);
                    }

                    audio_out_write(buf, solo);
                    ring_publish();
                    continue;
                }
            }
        }

        const size_t got = xStreamBufferReceive(s_ring[s_ring_play], buf,
                                                PCM_CHUNK_BYTES,
                                                pdMS_TO_TICKS(100));
        ring_publish();

        if (got) {
            /*
             * The gain, applied here and nowhere else.
             *
             * This is the last place the samples exist as samples before
             * the hardware has them, and it is on the task that owns the
             * read -- so a chunk cannot be modified between being taken
             * from the ring and being written, and there is no second
             * reader to disagree with. Everything a crossfade needs to
             * touch, it touches from inside this if.
             */
            bool ramp_over = false;
            if (s_fade_out) {
                fade_apply((int16_t *)buf, got / PCM_BYTES_PER_FRAME);
                ramp_over = (s_fade_pos >= s_fade_frames);
            }

            /*
             * The dip. Armed by the decode loop when it finds the next
             * track at a different rate; started here, when the ring
             * has come down to the length of the ramp, so it ends where
             * the audio does.
             */
            if (s_dip_dir == 0 && s_dip_arm) {
                const size_t avail =
                    xStreamBufferBytesAvailable(s_ring[s_ring_play]);
                if (avail <= (size_t)s_dip_arm * PCM_BYTES_PER_FRAME) {
                    s_dip_frames = (uint32_t)(avail / PCM_BYTES_PER_FRAME);
                    if (!s_dip_frames) s_dip_frames = 1;
                    s_dip_pos = 0;
                    s_dip_arm = 0;
                    s_dip_dir = -1;
                    ESP_LOGI(TAG, "rate change: fading out over %" PRIu32 " ms",
                             s_dip_frames * 1000u /
                             (s_frames_rate[s_ring_play] ?
                              s_frames_rate[s_ring_play] : 44100));
                }
            }
            if (s_dip_dir != 0 && !s_fade_out) {
                dip_apply((int16_t *)buf, got / PCM_BYTES_PER_FRAME);
                if (s_dip_pos >= s_dip_frames) s_dip_dir = 0;
            }

            audio_out_write(buf, got);

            /*
             * Written first, then dropped. The chunk that finished the
             * ramp ends at zero and is the last thing anybody hears;
             * what is behind it in the ring is up to twenty seconds of a
             * track that has already ended as far as the listener is
             * concerned, and clocking it out at a gain of zero would
             * mean twenty seconds of silence before the player would
             * answer with anything else.
             */
            if (ramp_over) {
                s_fade_out  = false;
                s_flush_why = "fade";
                s_pcm_flush = true;
                /* Nothing after this chunk is audible, so nothing on
                 * screen is describing anything that can still be
                 * heard. player_loop() does the drawing. */
                s_fade_cleanup = true;
                ESP_LOGI(TAG, "faded out over %" PRIu32 " ms", (uint32_t)FADE_OUT_MS);
            }
        } else if (s_fade_out) {
            /*
             * The ring ran dry mid-ramp.
             *
             * It should not often: the ramp is FADE_OUT_MS against a
             * ring that nominally holds twenty seconds. But it can --
             * the card can leave while the ring is still filling after
             * a seek, or on the first seconds of a track -- and the
             * result is simply that the audio ran out first. The tail
             * of the ramp is applied to nothing, which is what it would
             * have produced anyway.
             *
             * Cleared here regardless, because a fade left set is the
             * next track starting silent.
             */
            s_fade_out = false;
            /* Same conclusion by a different route: there is no audio
             * left, so the screen is stale either way. */
            s_fade_cleanup = true;
            ESP_LOGW(TAG, "fade: the ring emptied before the ramp finished");
        }
        /*
         * No exit, and no s_decode_done test any more.
         *
         * The writer used to end with the track, because the ring ended
         * with the track. Both now live for the program: an empty ring
         * means either that the decoder has not caught up or that
         * nothing is playing, and going round again is the answer to
         * both. The 100 ms receive timeout is what makes that cheap.
         *
         * This is the change that lets a track boundary happen without
         * the audio path being torn down and rebuilt around it, which
         * is what decoding the next track into a second ring needs.
         */
    }
    /* Not reached. buf is held for the life of the program deliberately;
     * a writer that freed it would be a writer that could stop. */
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
/*
 * Bumped on every pause. Read either side of a blocking send, so a
 * pause that arrives DURING one can be told from a genuine stall --
 * `!s_playing` sampled before the send cannot see a press that has not
 * happened yet, and sampled after it cannot see one that has already
 * been undone. See the ring send warning.
 */
static volatile uint32_t s_pause_epoch;
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
    case HID_BTN_PLAY_PAUSE:
        s_hid_action = UI_ACTION_PLAY_PAUSE;
        break;
    case HID_BTN_NEXT:
        s_hid_action = UI_ACTION_NEXT;
        break;
    case HID_BTN_PREV:
        /* The same as the on-screen button: start of track, and the
         * track before it only on a second press. That routes through
         * request_seek(0), which is a seek commit with nothing touching
         * the glass -- the hands-off test the flash investigation has
         * been unable to run. */
        s_hid_action = UI_ACTION_PREV;
        break;
    case HID_BTN_STOP:
        s_hid_action = UI_ACTION_PLAY_PAUSE;
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
/* The gear's request, and a request rather than a call for the same
 * reason the chooser's is: the press arrives partway through a UI
 * iteration that has already sampled a touch, and the screen change has
 * to happen at the top of the next one. */
static volatile bool     s_open_panel;

/* Set by the UI, consumed by the decode loop. -1 = nothing pending. Seek
 * is not implemented yet -- see README -- so the decode loop currently
 * logs and clears it. */
/*
 * The gain in effect for the track playing now, and whether there is
 * one. Published rather than fetched: the UI task cannot ask play_file()
 * anything -- the same rule s_ring_pct follows.
 */
static volatile bool     s_rg_measuring = false;
static volatile bool     s_rg_active = false;
static volatile float    s_rg_gain_db = 0.0f;

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
/*
 * Which track s_walk holds. media_task fills s_walk and raises
 * s_wave_ready; the decode loop draws it. Those are two tasks and two
 * tracks' worth of time apart, so without a name attached the flag says
 * only "an envelope is ready", not whose -- and an envelope prepared for
 * a track that was then skipped gets drawn over the one that replaced
 * it. Seen as a 427-column Bach envelope sitting on a 523-column
 * Couperin for five seconds.
 */
static char              s_wave_path[512];

/*
 * Which track's envelope is ON SCREEN, as opposed to which track has had
 * one prepared for it.
 *
 * THE BUG THIS EXISTS TO CLOSE.
 *
 * s_walked_path above means "the work of finding this track's envelope
 * has been done". That is a statement about media_task's effort, and it
 * was being used by track_change_begin() as though it also meant "the
 * envelope is drawn" -- which it does not, because the two are joined by
 * a handoff that only the decode loop consumes.
 *
 * So: let media_task load an envelope while NOTHING is playing. It
 * primes the sidecar, fills s_walk, sets s_wave_path and raises
 * s_wave_ready, and records the path in s_walked_path. There is no
 * decode loop to consume the handoff, so nothing is drawn. Press play on
 * that same track and track_change_begin() clears s_wave_ready (rightly
 * -- a stale handoff must not survive a track change) and then skips the
 * install because s_walked_path already names this path. The envelope
 * exists in the cache, has been paid for, and is never drawn for the
 * whole song.
 *
 * It needs an idle gap long enough for media_task to finish -- which is
 * exactly what sitting on the settings panel for twenty seconds
 * produces, and is why it surfaced when it did rather than being a new
 * fault.
 *
 * The fix is to stop asking the wrong variable. This one is written only
 * where waveform_set() is actually called, so it cannot drift from what
 * the bar is drawing.
 */
static char              s_wave_shown[512];

/*
 * The path whose envelope is held in s_walk_pending, waiting for the
 * handoff. Needed for the same reason s_wave_path is: by the time
 * track_change_show() runs, `path` is out of scope and the pending
 * envelope has to be able to say whose it is.
 */
static char              s_env_path[512];

/* The two ways the bar's envelope changes, and the only two. Every
 * waveform_set() call in this file goes through one of them, so
 * s_wave_shown cannot fall out of step with what is drawn. */
static void wave_show(const framewalk_t *w, const char *path)
{
    waveform_set(w);
    snprintf(s_wave_shown, sizeof(s_wave_shown), "%s", path ? path : "");
}

static void wave_clear(void)
{
    waveform_set(NULL);
    s_wave_shown[0] = '\0';
}

/*
 * Is something else occupying the whole screen?
 *
 * The art path blits straight to the panel -- albumart_show(),
 * ui_clear_art() and the format card all bypass ui_draw() -- so every
 * one of those sites has to know whether there is a transport bar to
 * draw on. Four of them asked browser_is_open(), which was the complete
 * answer while the chooser was the only full-screen thing in the
 * program.
 *
 * The settings panel is the second, and it inherited none of the
 * guards: a track change or a cover landing while the panel was up
 * painted the art strip across it. This is that answer, asked once, so
 * that the third full-screen thing is one line here rather than another
 * four sites nobody remembers to update.
 *
 * s_repaint_art is what brings the artwork back, and both screens set it
 * on the way out.
 */
static bool screen_covered(void)
{
    return browser_is_open() || panel_is_open();
}

/*
 * The track's sidecar record, held open for the length of the track.
 *
 * Every fact learned during an open -- tags, whether there is a cover,
 * the format the decoder reports -- used to be written the moment it
 * was found, and each write was a load, a merge and a full rewrite. A
 * log showed the cost: three saves during one open meant three loads
 * and three rewrites of the same kilobyte within five seconds, on top
 * of the read play_file() had already done to get the gain.
 *
 * So it is read once, merged into in memory, and written once when the
 * track ends. s_rg_dirty is what says a write is owed; without it a
 * track that taught us nothing new would rewrite its sidecar on every
 * play for ever.
 *
 * Owned by the decode loop. media_task does its own short-lived loads
 * for the prefetch and does not touch this.
 */
static replaygain_t      s_rg;
static bool              s_rg_dirty;
static char              s_rg_path[512];
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
/*
 * The replaygain-hold helpers and the sidecar primer, declared here
 * because load_tags() and track_change_begin() call all three several
 * hundred lines above where they are defined. C89 would have taken the
 * implicit int() declarations; anything since is right to refuse them,
 * and a prototype is cheaper than moving the definitions up past the
 * statics they read.
 */
static void rg_hold(const char *path);
static bool rg_holding(const char *path);
static bool sidecar_prime(const char *path);

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
 * THE TITLE WAITS FOR THE COVER.
 *
 * The two arrive on different tasks: load_tags() runs on the decode
 * loop at the track change, do_art() on media_task some way behind it.
 * Published as they landed, the screen changed twice -- the new title
 * against the old cover, and then the cover -- which reads as the
 * player having got the title wrong first, or as the art being late for
 * a track that has already started.
 *
 * So the text is STAGED here and shown when the art for the same track
 * has settled. `_shown` is what the UI reads; `s_tags` and
 * s_display_name remain what the loaders write, so nothing else in this
 * file changes.
 *
 * Copied by the UI task itself rather than published by media_task,
 * which would be a struct written by one task and read by another with
 * no ordering -- a torn title for a frame. The UI task is the only
 * reader, so letting it do the copy makes the question disappear rather
 * than answering it.
 */
static id3_tags_t   s_tags_shown;
static const char  *s_name_shown = "";
static volatile bool      s_text_staged;
static volatile bool      s_text_release;
static volatile TickType_t s_text_staged_at;

/*
 * How long the text will wait for a cover before showing anyway.
 *
 * do_art() can decline entirely -- the chooser is up, the generation
 * moved on, the file would not open -- and none of those should cost
 * the listener a title. A second is longer than the art path takes
 * whenever it runs at all, and short enough that the failure mode is a
 * slightly late title rather than a missing one.
 */
#define TEXT_HOLD_MS    (1000)

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
                /* Into the held record, not to the card: the write
                 * happens once when the track ends. */
                if (rg_holding(path) && !s_rg.tags.present) {
                    s_rg.tags.present = true;
                    snprintf(s_rg.tags.title,  sizeof(s_rg.tags.title),  "%s", s_tags.title);
                    snprintf(s_rg.tags.artist, sizeof(s_rg.tags.artist), "%s", s_tags.artist);
                    snprintf(s_rg.tags.album,  sizeof(s_rg.tags.album),  "%s", s_tags.album);
                    s_rg_dirty = true;
                }
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

    /* Staged, not shown: see s_tags_shown. */
    s_text_staged = true;
    s_text_release = false;
    s_text_staged_at = xTaskGetTickCount();

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

/* Whether track_change_begin() left an envelope undrawn because the
 * previous track was still playing, and if so which kind. */
enum { ENV_PENDING_NONE = 0, ENV_PENDING_SET, ENV_PENDING_CLEAR };
static int s_env_pending = ENV_PENDING_NONE;

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
    /*
     * ...but only when there is no previous track still being heard.
     *
     * With a decode-ahead this function runs a ring BEFORE the new
     * track's first sample -- twenty seconds during which the old track
     * is still playing and its numbers are still the true ones. Retiring
     * them here blanked the bar under a song that was still going, and
     * the gate then republished them for a track that had not started:
     * the bar jumped to the next song early and then sat still. When the
     * writer is still on the other ring these are left alone and the
     * gate replaces them at the handoff, which is when the screen and
     * the sound change together.
     */
    if (!tail_playing()) {
        s_stats_valid = false;
        s_pos_sec = 0;
        s_len_sec = 0;
        s_can_seek = false;
    }
    s_fmt_known = false;
    /*
     * The record first, before anything that could go and read the file
     * for something it already holds.
     *
     * This used to sit inside play_file(), which runs after this
     * function -- so load_tags() below re-read the ID3 and do_art()
     * re-scanned for a cover that the sidecar already said was absent,
     * and both then wrote back what was already there. The seeding was
     * real and simply one step downstream of its consumers.
     */
    rg_hold(path);
    const bool primed_here = sidecar_prime(path);

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
    /*
     * Against what is DRAWN, not against what has been looked up. See
     * s_wave_shown -- this test used to be strcmp(path, s_walked_path),
     * which is true for a track media_task prepared an envelope for
     * while nothing was playing, and the preparation is not the drawing.
     */
    if (strcmp(path, s_wave_shown) != 0) {
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
            /* Held rather than drawn when the previous track is still
             * playing: this envelope belongs to a song nobody is
             * hearing yet. track_change_show() draws it at the handoff.
             * The buffer is safe to hold -- see the note above on why it
             * is not s_walk. */
            if (!tail_playing()) {
                wave_show(&s_walk_pending, path);
            } else {
                s_env_pending = ENV_PENDING_SET;
                snprintf(s_env_path, sizeof(s_env_path), "%s", path);
            }
            snprintf(s_walked_path, sizeof(s_walked_path), "%s", path);
            /* Quiet when sidecar_prime() a few lines up is what put it
             * there: two lines describing one lookup reads as two. */
            if (!primed_here) {
                ESP_LOGI(TAG, "envelope from cache at track change");
            }
        } else {
            if (!tail_playing()) wave_clear();
            else                 s_env_pending = ENV_PENDING_CLEAR;
        }
    }

    /* Read before the screen is asked to show anything, so the title row
     * is either right or empty and never a filename standing in for a
     * title the file actually has.
     *
     * Skipped entirely during a decode-ahead: s_tags is what the title
     * row draws, so writing it here would put the next song's name over
     * the one still playing. load_track_visuals() calls load_tags() at
     * the handoff and the cache makes that call free, so nothing is lost
     * but the twenty seconds of lying. */
    if (!tail_playing()) {
        load_tags(path);
        ui_clear_art();
    }
}

/*
 * The half of a track change that is deferred to the handoff.
 *
 * Only the envelope: the tags, the cover and the art strip are already
 * load_track_visuals()' job and the gate calls that too. Kept next to
 * the flag it consumes rather than inside the gate macro so there is one
 * place to look for what a deferred change still owes the screen.
 */
static void track_change_show(void)
{
    if (s_env_pending == ENV_PENDING_SET)        wave_show(&s_walk_pending, s_env_path);
    else if (s_env_pending == ENV_PENDING_CLEAR) wave_clear();
    s_env_pending = ENV_PENDING_NONE;
}

static void load_track_visuals(const char *path)
{
    /*
     * Not while the chooser is up.
     *
     * Everything below this blits straight to the panel -- albumart_show()
     * decodes a cover into the art strip, ui_clear_art() paints it out,
     * ui_show_art_info() writes the fallback text. None of it goes
     * through ui_draw(), which is what the browser branch in media_task
     * skips with its `continue`, so a track change while the list is open
     * painted the art strip over the top of it: a band of cover or of
     * black across rows that were showing filenames a moment earlier.
     *
     * Skipped rather than queued, and s_repaint_art is what brings it
     * back. Every browser exit already sets that flag -- play, play
     * folder and cancel alike -- so the cover is drawn once, when there
     * is a transport bar to draw it on. A track change with the chooser
     * open is exactly the case that flag was added for; it just was not
     * being consulted on this side.
     */
    if (screen_covered()) {
        s_repaint_art = true;
        return;
    }

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
        /* The chooser can be opened during the wait, which on a
         * Xing-less MP3 is seconds long. Checked every slice for the
         * same reason gen is: this loop is the one place in the art path
         * that spends real time, so it is where the world changes. */
        if (screen_covered()) {
            s_repaint_art = true;
            return;
        }
    }
    if (gen != s_track_gen) return;

    /*
     * And again on the way out, because the two tests above cover
     * different things: gen says this is still the right track, this
     * says there is somewhere to draw it. ui_show_art_info() blits
     * straight to the panel -- it does not go through ui_draw(), which
     * is what media_task's browser branch skips -- so without this the
     * format card lands on top of the file list.
     *
     * The cover had this guard, in load_track_visuals(). The text that
     * stands in for a missing cover did not, and it comes through
     * media_task by a different route.
     */
    if (screen_covered()) {
        s_repaint_art = true;
        return;
    }

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
     * Not while the chooser is up -- the same guard, and the same
     * reasoning, as the one in load_track_visuals().
     *
     * This is the other way into the art strip. That one is the repaint
     * path and runs on the decode loop; this one runs on media_task at
     * a track change, and it was never guarded. albumart_show(),
     * ui_clear_art() and show_format_card() all blit straight to the
     * panel from here.
     *
     * s_repaint_art brings it back when the chooser closes, which every
     * browser exit already sets.
     */
    if (screen_covered()) {
        s_repaint_art = true;
        return;
    }

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
            /* And into the held record, so the next boot does not pay
             * the scan again -- the eight to thirteen seconds the logs
             * show before this line. Written when the track ends. */
            if (rg_holding(path) && !s_rg.art.present) {
                s_rg.art.present = true;
                s_rg.art.has_art = false;
                s_rg_dirty = true;
            }
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
/*
 * Seed every RAM cache from the track's sidecar, in one read.
 *
 * The caches already have the gates -- mediacache_walk(),
 * mediacache_tags(), mediacache_no_art() are each consulted before the
 * expensive thing that would otherwise produce the answer. This fills
 * them from disk so those gates hit on a track that has been played
 * before, and the expensive things never run:
 *
 *   envelope   otherwise nothing until a full play finishes
 *   tags       otherwise an ID3 read on every open
 *   "no art"   otherwise a whole tag scan to learn the same nothing,
 *              which the logs show landing eight to thirteen seconds in
 *
 * One replaygain_load() for all of it, because the record is one line
 * and reading it three times would be three opens for the same 1 KB.
 *
 * Deliberately does NOT seed a positive cover. The sidecar stores where
 * the image is, not the image; using that means a seek and a read,
 * which belongs on the art path where the decode already happens, not
 * here where it would block the open. The definite negative is the
 * valuable half anyway -- it turns a scan into nothing at all.
 *
 * Returns true when the envelope specifically is now cached, which is
 * the question the seek bar's caller asks.
 */
/*
 * The record handed to media_task to write, and the flag that transfers
 * ownership of it. Declared here rather than beside the writer because
 * rg_hold() below reads them: a record still in flight is newer than
 * the file it is on its way to.
 *
 * One slot, one producer (the decode loop, in rg_release()), one
 * consumer (media_task). The producer allocates, fills, and sets the
 * flag LAST; the consumer writes, frees, and clears the flag LAST, so
 * neither touches the other's allocation and no lock is needed.
 */
static replaygain_t *s_rg_pending;
static char          s_rg_pending_path[512];
static volatile bool s_rg_pending_ready;

/*
 * Start holding `path`'s record. One read; everything the open is about
 * to want is in memory afterwards.
 */
static void rg_hold(const char *path)
{
    /*
     * THE COPY IN FLIGHT BEATS THE ONE ON THE CARD.
     *
     * rg_release() hands the record to media_task and the write lands
     * a second or so later, by which time the next open has already
     * read the file -- so a track played twice in a row reads back the
     * version from before the play that just ended.
     *
     * Invisible until repeat-one existed, and then immediately visible:
     * `length from playback: 59 s` followed by `no duration available`
     * on the very next open of the same file, the measurement having
     * been overwritten by the stale copy it was about to replace.
     *
     * Taking the pending record when the path matches is not a cache --
     * it is the same record, one step earlier in its journey to the
     * same place.
     */
    if (s_rg_pending_ready && s_rg_pending &&
        strcmp(s_rg_pending_path, path) == 0) {
        memcpy(&s_rg, s_rg_pending, sizeof(s_rg));
        snprintf(s_rg_path, sizeof(s_rg_path), "%s", path);
        s_rg_dirty = false;
        ESP_LOGI(TAG, "sidecar: from the record still being written");
        return;
    }

    if (!replaygain_load(path, &s_rg)) memset(&s_rg, 0, sizeof(s_rg));
    snprintf(s_rg_path, sizeof(s_rg_path), "%s", path);
    s_rg_dirty = false;
}

/*
 * Write it back, if anything changed. Called when the track ends,
 * whatever the reason -- a skipped track still learned its tags and
 * whether it has a cover, and those are worth keeping even though its
 * loudness is not.
 */
/* When the hand-off happened, for the deadline below. 0 means the wait
 * has not started. */
static TickType_t    s_rg_pending_since;

/*
 * Hand the record to media_task rather than writing it here.
 *
 * This ran inline at the end of play_file() and was, by the end, the
 * entire gap between two tracks:
 *
 *   I (341534) tab5_mp3: finished, 15930 blocks
 *   I (343098) tab5_rg: sidecar written: ... (994 bytes)
 *   I (343100) tab5_mp3: playing 08 - Toki on White Waves
 *
 * 1.56 s to write 994 bytes, because it is not a write -- it is a
 * remove, a rename, and a create, three FAT metadata operations on a
 * USB drive, with the next track's open queued behind all of them.
 *
 * media_task is priority 1, does this kind of IO already, and nothing
 * waits on it. The copy costs 3 KB of PSRAM for as long as the write
 * takes.
 *
 * Two fallbacks to inline, both rare and both better than losing the
 * record: no memory for the copy, and a previous hand-off still
 * unclaimed. The second means two tracks ended within one media_task
 * poll of each other, which takes a skip landing in exactly the wrong
 * 50 ms.
 */
static void rg_release(void)
{
    if (s_rg_dirty && s_rg_path[0]) {
        replaygain_t *copy = s_rg_pending_ready
                             ? NULL
                             : heap_caps_malloc(sizeof(*copy),
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (copy) {
            memcpy(copy, &s_rg, sizeof(*copy));
            snprintf(s_rg_pending_path, sizeof(s_rg_pending_path), "%s", s_rg_path);
            s_rg_pending = copy;
            s_rg_pending_ready = true;      /* last: this is the hand-off */
        } else {
            /* Whatever the reason, the record is this track's and is
             * worth more than the boundary is. */
            replaygain_write(s_rg_path, &s_rg);
        }
    }
    s_rg_dirty = false;
    s_rg_path[0] = '\0';
}

/* media_task's half. Called at the top of its loop, so the write starts
 * within one poll of the track ending and runs alongside the next
 * track's open rather than in front of it. */
/*
 * How full the ring has to be before the deferred write is allowed to
 * start, and how long that requirement is honoured.
 *
 * Deferring the write to media_task moved it off the track boundary and
 * straight into the next track's fill:
 *
 *   I (343486) tab5_mp3: open took 5 ms
 *   I (344801) tab5_rg: sidecar written: ... (984 bytes)
 *   I (344881) tab5_mp3: first sound 1464 ms after the press
 *
 * Same silence, one step later. A write is only free when playback has
 * something banked, so it waits for the ring.
 *
 * The deadline is the other half. On a slow volume the ring may never
 * reach the mark, and a record that is never written means the track is
 * measured again on every play for ever -- which is the thing the
 * sidecar exists to stop. Ten seconds of trying, then write regardless.
 */
#define RG_WRITE_MIN_RING_PCT   (40)
#define RG_WRITE_DEADLINE_MS    (10000)

static void rg_write_pending(void)
{
    if (!s_rg_pending_ready) return;

    /*
     * -1 is no ring at all, which means nothing is playing and there is
     * nobody to get in the way.
     */
    const int pct = s_ring_pct;
    if (pct >= 0 && pct < RG_WRITE_MIN_RING_PCT) {
        if (s_rg_pending_since == 0) s_rg_pending_since = xTaskGetTickCount();
        if ((int32_t)(xTaskGetTickCount() - s_rg_pending_since) <
            (int32_t)pdMS_TO_TICKS(RG_WRITE_DEADLINE_MS)) {
            return;
        }
        ESP_LOGW(TAG, "sidecar write went ahead at ring %d%%", pct);
    }
    s_rg_pending_since = 0;

    replaygain_t *rg = s_rg_pending;
    if (rg) replaygain_write(s_rg_pending_path, rg);
    s_rg_pending = NULL;
    heap_caps_free(rg);
    s_rg_pending_ready = false;             /* last: the slot is free again */
}

/* True when the held record is this track's. Guards every merge below:
 * a fact learned about one track must not be filed under another. */
static bool rg_holding(const char *path)
{
    return path && s_rg_path[0] && strcmp(s_rg_path, path) == 0;
}

/*
 * The two big records here are heap, not stack.
 *
 * A replaygain_t is a shade over 3 KB -- the seek index alone is 256
 * offsets and 256 sample positions -- and a framewalk_t is another
 * kilobyte. As automatics they made this one frame about 4.5 KB of
 * media_task's 8 KB stack, and replaygain_load() has records of its own
 * below it. That overflowed on the second track: the frame was always
 * this size, and the prefetch simply happened to be the call that ran
 * out of room.
 *
 * PSRAM because neither record is touched by DMA and nothing here is in
 * the audio path. A failed allocation is not fatal -- it means the same
 * as a sidecar that would not load, which this already has an answer
 * for.
 */
static bool sidecar_prime(const char *path)
{
    const bool have_walk = (mediacache_walk(path) != NULL);
    id3_tags_t dummy;
    const bool have_tags = mediacache_tags(path, &dummy);
    const bool have_art  = mediacache_no_art(path) ||
                           (mediacache_art(path, NULL) != NULL);

    /* Everything already in RAM: no read at all. */
    if (have_walk && have_tags && have_art) return true;

    /*
     * The held record when there is one, and only then a load.
     *
     * rg_hold() runs on the line above the sidecar_prime() call in
     * track_change_begin() and has the same record in memory already;
     * reading the file again here was the read-it-twice shape the
     * writes had before 0211, just cheaper.
     *
     * The other caller is media_task priming the NEXT track during
     * prefetch. Nothing is held for that one, so it loads -- into
     * PSRAM, because a replaygain_t is a shade over 3 KB and an
     * automatic here is what overflowed media_task's stack.
     */
    replaygain_t *loaded = NULL;
    const replaygain_t *rg;

    if (rg_holding(path)) {
        rg = &s_rg;
    } else {
        loaded = heap_caps_malloc(sizeof(*loaded),
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!loaded) {
            ESP_LOGW(TAG, "no room for a sidecar record; not priming");
            return have_walk;
        }
        if (!replaygain_load(path, loaded)) {
            heap_caps_free(loaded);
            return have_walk;
        }
        rg = loaded;
    }

    if (!have_walk && rg->waveform.present && rg->waveform.columns > 0) {
        framewalk_t *w = heap_caps_malloc(sizeof(*w),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (w) {
            memset(w, 0, sizeof(*w));
            w->has_levels = true;
            w->columns = rg->waveform.columns;
            w->sec     = rg->waveform.sec;
            memcpy(w->level, rg->waveform.level, (size_t)rg->waveform.columns);
            mediacache_put_walk(path, w);
            ESP_LOGI(TAG, "envelope from sidecar: %d columns, %" PRIu32 "s",
                     w->columns, w->sec);
            heap_caps_free(w);
        }
    }

    if (!have_tags && rg->tags.present) {
        id3_tags_t t;
        memset(&t, 0, sizeof(t));
        snprintf(t.title,  sizeof(t.title),  "%s", rg->tags.title);
        snprintf(t.artist, sizeof(t.artist), "%s", rg->tags.artist);
        snprintf(t.album,  sizeof(t.album),  "%s", rg->tags.album);
        mediacache_put_tags(path, &t);
    }

    /* Only the negative, and only when it is a positive statement that
     * there is none -- art.present with has_art false. An absent
     * section means nobody has looked, which is not the same claim. */
    if (!have_art && rg->art.present && !rg->art.has_art) {
        mediacache_put_no_art(path);
        ESP_LOGI(TAG, "no cover art (sidecar)");
    }

    const bool ret = have_walk ||
                     (rg->waveform.present && rg->waveform.columns > 0);
    heap_caps_free(loaded);        /* NULL when the record was the held one */
    return ret;
}



/*
 * File a format section for a track whose container would not say.
 *
 * WHY THIS SECTION EXISTS AT ALL, given the waveform section already
 * carries a `sec`.
 *
 * Because that one is a side effect. The envelope's duration is
 * whatever the frame walk happened to reach, filed under a section that
 * is about drawing a picture, and it is only there when an envelope was
 * wanted -- so a format with no stated length has its duration recorded
 * or not depending on whether the bar wanted a waveform that day. The
 * format section is the place where "this file is 203 seconds long" is
 * a fact about the file rather than a property of a drawing of it.
 *
 * WHAT IS WORTH WRITING
 *
 * Only what was expensive to find. A Xing-less MP3 pays for a whole-file
 * index build; raw ADTS and AMR pay for a frame walk. Those are the
 * files this is for -- the ones where the answer cost seconds of card
 * reads and is otherwise thrown away at the end of every play. A FLAC
 * states its length in its header and does not need a copy of it in a
 * sidecar.
 *
 * The bitrate goes in alongside because it is free once the duration is
 * known and because the alternative is what the log currently shows: an
 * Ogg drawing "0 kbps" on the format card, which is not a measurement,
 * it is the absence of one printed as though it were.
 *
 * Merged into the held record rather than written here. rg_release()
 * hands the whole thing to media_task at the track boundary, which is
 * the one place a card write does not sit in front of a decode.
 */
static void rg_note_format(const char *path, const decoder_info_t *info,
                           uint32_t len_sec, uint64_t file_bytes)
{
    if (!rg_holding(path) || !info || len_sec == 0) return;
    if (s_rg.format.present && s_rg.format.sec == len_sec) return;

    s_rg.format.present     = true;
    s_rg.format.sec         = len_sec;
    s_rg.format.sample_rate = (uint32_t)info->sample_rate;
    s_rg.format.channels    = (uint8_t)info->channels;
    snprintf(s_rg.format.codec, sizeof(s_rg.format.codec), "%s",
             info->codec ? info->codec : "");

    /*
     * The decoder's figure when it has one, otherwise the file over its
     * own duration.
     *
     * Deliberately the average and not a nominal: a Vorbis identification
     * header states 96 kbps for a stream that may never sustain it, and
     * on a VBR file the number people actually want is what it came out
     * at. It is a couple of percent high because the container's own
     * overhead is in the byte count -- which is why the format card
     * marks it as approximate rather than pretending to four digits.
     */
    if (info->bitrate_kbps > 0) {
        s_rg.format.kbps = (uint16_t)info->bitrate_kbps;
    } else if (file_bytes) {
        const uint64_t bps = (file_bytes * 8) / len_sec;
        s_rg.format.kbps = (uint16_t)((bps + 500) / 1000);
    }

    /*
     * From 0602, which is where the player learned to ask. EXACT is the
     * only answer that means the ends have been dealt with; UNKNOWN and
     * NONE both mean they have not, and storing that stops the next open
     * re-checking a file that has already been found to say nothing.
     *
     * The delay and padding counts stay zero: minimp3_ex applies them
     * internally and does not hand them back, so there is nothing
     * truthful to put there yet.
     */
    s_rg.format.has_gapless = (info->trim == DECODER_TRIM_EXACT);

    s_rg_dirty = true;
    ESP_LOGI(TAG, "filed format: %" PRIu32 "s, %u kbps, trim %s",
             len_sec, (unsigned)s_rg.format.kbps,
             decoder_trim_name(info->trim));
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
static void pos_publish(int play, size_t queued)
{
    const uint32_t rate = s_frames_rate[play];
    if (!rate) return;

    const uint64_t queued_frames = queued / PCM_BYTES_PER_FRAME;
    const uint64_t decoded = s_frames_out[play];
    const uint64_t heard = decoded > queued_frames ? decoded - queued_frames : 0;
    s_pos_sec = (uint32_t)(heard / rate);
}

static void ring_publish(void)
{
    /*
     * Two rings, two questions, and they are not the same question.
     *
     * Occupancy is asked by the prefetch and by media_task, and what
     * they want to know is whether the DECODER has headroom -- so it is
     * measured on the ring being filled, s_pcm.
     *
     * Position is asked by the display, and what it wants to know is
     * where the audible track has got to -- so it is measured on the
     * ring being played, which during a decode-ahead is the other one
     * entirely. Measuring the position against the fill ring was the
     * second half of the twenty-second bug: the numbers came from two
     * different tracks and the difference between them meant nothing.
     */
    const size_t fill_queued = xStreamBufferBytesAvailable(s_pcm);
    s_ring_pct = (int)((fill_queued * 100) / PCM_RING_BYTES);

    const int play = s_ring_play;
    pos_publish(play, xStreamBufferBytesAvailable(s_ring[play]));

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
 * cost a few KB, which a device can spare.
 *
 */

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
    /*
     * Just a sidecar read now. There is no prefetch walk any more: the
     * envelope is a by-product of playing a track, so a track nobody has
     * played has none to fetch and one that has been played has it in a
     * dotfile. This warms the RAM cache so the track change finds it.
     */
    sidecar_prime(next);

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
        /* Before the want check, so a sidecar handed over at a track
         * boundary is written even while the next track's cover work is
         * queued behind it. */
        rg_write_pending();

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

        /* Whatever it decided -- a cover, a format card, or nothing at
         * all because the generation moved on -- the art strip is now
         * as settled as it is going to get for this track, so the title
         * can go with it. */
        s_text_release = true;

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
        /*
         * The envelope, from the sidecar if the track has been played
         * before. No settle: this is a small read, not the whole-file
         * scan the wait was built around.
         */
        if (strcmp(path, s_walked_path) != 0) {
            if (sidecar_prime(path)) {
                const framewalk_t *hit = mediacache_walk(path);
                if (hit) {
                    memcpy(&s_walk, hit, sizeof(s_walk));
                    snprintf(s_wave_path, sizeof(s_wave_path), "%s", path);
                    s_wave_ready = true;
                }
            }
            snprintf(s_walked_path, sizeof(s_walked_path), "%s", path);
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
    /* A refill pace belongs to the track it was started in. The new
     * track's own fill is a tail decode-ahead and is paced as one. */
    s_refill_pacing = false;

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

        /*
         * The panel, on the same terms as the chooser above it.
         *
         * Ordered after the chooser's open branch and before its touch
         * branch, so the two screens cannot both be up: the gear is on
         * the transport bar and unreachable while the chooser is up, and
         * this branch returns before the chooser's would run.
         *
         * The swallow and the draw-then-continue are the chooser's, for
         * the chooser's reason: bdown was sampled at the top of this
         * iteration, before the swallow existed, so falling through
         * hands the panel the press that opened it.
         */
        if (s_open_panel) {
            s_open_panel = false;
            touch_swallow();
            panel_open();
            panel_draw();
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (panel_is_open()) {
            if (panel_touch(bdown, bx, by)) {
                panel_close();
                touch_swallow();
                /* The bar underneath has been painted over, so it has to
                 * be redrawn from scratch rather than updated -- the same
                 * repaint the chooser asks for on its way out. */
                s_repaint_art = true;
            } else {
                panel_draw();
            }
            vTaskDelay(pdMS_TO_TICKS(bdown ? 20 : 100));
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

        /*
         * The staged text becomes the shown text when the art has
         * settled, or when it has waited long enough that no art is
         * coming. Done here because this task is the only reader.
         */
        if (s_text_staged) {
            const bool waited =
                (int32_t)(xTaskGetTickCount() - s_text_staged_at) >=
                (int32_t)pdMS_TO_TICKS(TEXT_HOLD_MS);
            if (s_text_release || waited) {
                memcpy(&s_tags_shown, &s_tags, sizeof(s_tags_shown));
                s_name_shown = s_display_name;
                s_text_staged = false;
                s_text_release = false;
            }
        }

        st.title = s_tags_shown.title[0] ? s_tags_shown.title : s_name_shown;
        st.artist = s_tags_shown.artist;
        st.album = s_tags_shown.album;
        st.playing = s_playing;
        st.volume = s_volume;
        st.rg_active = s_rg_active;
        st.rg_measuring = s_rg_measuring;
        st.rg_gain_db = s_rg_gain_db;
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
        /*
         * And not while a tail is still playing.
         *
         * s_decoding and s_track_changing between them used to cover
         * every moment audio could be coming out, because a decode loop
         * that had ended meant a ring that had been drained. The last
         * track of a folder broke that: nothing is decoding, nothing is
         * changing, and twenty seconds of music is still queued -- so
         * the amplifier powered down 1.5 s after the decode ended and
         * the album stopped being audible while the screen, correctly,
         * went on showing it playing.
         *
         * A tail is the third way for sound to exist. It belongs in the
         * same condition as the other two.
         */
        /*
         * And not during a fade, which is the fourth way for sound to
         * exist and the only one that happens with the decode loop
         * already returned and the tail deliberately unlatched.
         *
         * Without this the media-gone path is a race between the ramp
         * and IDLE_HOLD_MS: nothing is decoding, nothing is changing,
         * no tail is pending, so this line cuts the amplifier partway
         * down the ramp and the fade this patch exists to add ends as
         * the click it was meant to replace.
         */
        audio_out_set_idle(!s_playing ||
                           (!s_decoding && !s_track_changing &&
                            !s_tail_pending && !fade_out_active()));
        st.battery_pct = battery_pct();
        st.battery_charging = battery_charging();
        st.ext_power = battery_external();

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
            /*
             * With nothing decoding, this is the press that starts the
             * restored track rather than a pause toggle. s_playing is
             * the writer's gate and toggling it with no producer would
             * silently do nothing, which reads as a dead button.
             */
            if (!s_decoding && s_path[0]) {
                if (!s_playing) s_playing = true;
                request_track(s_path);
                break;
            }
            s_playing = !s_playing;
            if (!s_playing) s_pause_epoch++;
            break;
        case UI_ACTION_VOLUME:
            /* 0503: logged because it was the tell. Seven unbidden volume
             * changes in one capture were the first evidence of presses
             * nobody made, and until then the only trace of them was the
             * settings file being rewritten. */
            ESP_LOGI(TAG, "button: volume -> %d%%", act.value);
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
            /* Repeat-one for the same reason as ONE: it is an answer
             * about the end of a track, and this is a press. Left
             * as-is, next would replay the track the listener just
             * asked to leave. */
            const char *p = playlist_next(
                (ord == PLAY_ORDER_ONE || ord == PLAY_ORDER_REPEAT_ONE)
                ? PLAY_ORDER_ALL : ord);
            if (p) request_track(p);
            else   ESP_LOGI(TAG, "no next track in %s", playlist_dir());
            break;
        }
        case UI_ACTION_CHOOSE_FILE:
            s_open_chooser = true;
            break;
        case UI_ACTION_SETTINGS:
            s_open_panel = true;
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
        st.rg_active = s_rg_active;
        st.rg_measuring = s_rg_measuring;
        st.rg_gain_db = s_rg_gain_db;
        st.muted = audio_out_muted();
        st.pos_sec = s_pos_sec;
        st.len_sec = s_len_sec;
        st.can_seek = s_can_seek;
        st.screen_off = s_screen_off;
        st.stats_valid = s_stats_valid;
        st.battery_pct = battery_pct();
        st.battery_charging = battery_charging();
        st.ext_power = battery_external();
        ui_draw(&st);

        /* 50 Hz under a finger, 25 Hz while the title is travelling, 10 Hz
         * otherwise. The middle rate exists because a marquee stepped at
         * 10 Hz does not read as movement, it reads as a title jumping
         * three pixels at a time; ui_animating() is false the moment the
         * title fits, so a short one costs nothing. */
        /*
         * The underrun counter, once a second and only when it has
         * moved. Silence means the DPI is keeping up; a line here next
         * to a flash is the confirmation five patches of bandwidth work
         * have been assuming, and a flash with no line here means those
         * patches were aimed at the wrong thing entirely.
         */
        {
            static uint32_t   last_underruns;
            static TickType_t last_report;
            const TickType_t now = xTaskGetTickCount();
            if (pdTICKS_TO_MS(now - last_report) >= 1000) {
                const uint32_t n = g_tab5_dpi_underruns;
                if (n != last_underruns) {
                    ESP_LOGW(TAG, "DSI underruns: %" PRIu32 " (+%" PRIu32 ")",
                             n, n - last_underruns);
                    last_underruns = n;
                }
                last_report = now;
            }
        }

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

/*
 * Where the previous track was and how it ended.
 *
 * Written at the bottom of play_file(), read at the top of the next
 * one. Two variables rather than one because they answer different
 * questions and a boundary can fail either: a skip within an album and
 * a clean end between albums are both "no crossfade" for unrelated
 * reasons.
 */
static char s_prev_dir[512];
/* The whole path, not just the folder. See the arming block: a track
 * cannot be crossfaded into itself. */
static char s_prev_path[512];
static bool s_prev_ended_clean;

/*
 * Same album means same folder.
 *
 * Not the same ALBUM tag, and that is a decision rather than an
 * approximation. A folder is what the playlist was built from and what
 * the listener actually picked; a tag is often missing entirely -- the
 * Ogg rips this player is full of have no ALBUM field at all, so a
 * tag-based test would call every one of them its own album and
 * crossfade a record that was cut to run together.
 *
 * Compared on the directory part only, which is what the playlist walks.
 * A subfolder is a different album by this test, which is right for
 * discs and wrong for nothing much.
 */
static bool same_album(const char *a, const char *b)
{
    if (!a || !b || !b[0]) return false;

    const char *sa = strrchr(a, '/');
    if (!sa) return false;

    const size_t len = (size_t)(sa - a);
    return strlen(b) == len && strncmp(a, b, len) == 0;
}

static track_end_t play_file(const char *path)
{
    const storage_id_t vol = storage_of_path(path);

    /* The duration the sidecar already knows, if it does. Held as a
     * local because it is learned before the decoder is open and shown
     * only once this track is the audible one. */
    uint32_t seed_len = 0;

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

    /*
     * The seek table, if this file has one on the card.
     *
     * s_rg was loaded in track_change_begin(), before this, for exactly
     * this class of reason: everything the open would otherwise have to
     * find out for itself is already in hand. With a table, minimp3
     * opens with MP3D_DO_NOT_SCAN and does not walk the file -- which on
     * a Xing-less MP3 is the 1.2 to 1.8 seconds that is now essentially
     * the whole of the open.
     *
     * Built as a local view rather than passed as the sidecar struct,
     * because decoder.c has no business knowing what a replaygain_t is.
     */
    decoder_index_t ix = {0};
    const bool have_ix = rg_holding(path) && s_rg.index.present &&
                         s_rg.index.count > 1;
    if (have_ix) {
        ix.count = s_rg.index.count;
        ix.spacing_sec = s_rg.index.spacing_sec;
        ix.offset = s_rg.index.offset;
        ix.frame = s_rg.index.sample;
    }

    const int64_t t_open = esp_timer_get_time();
    decoder_t *dec = decoder_open_indexed(path, have_ix ? &ix : NULL);
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

    /*
     * Whether this track may be crossfaded into.
     *
     * Decided here because everything in it is policy the writer cannot
     * see: how the PREVIOUS track ended, and whether the two are from
     * the same album. The writer decides whether an overlap actually
     * happens -- rates, channel counts and how much is queued are its
     * business and not this one's.
     *
     * s_prev_ended_clean is the important half. A crossfade belongs to a
     * track that finished; a skip, a seek that ran off the end, an
     * unreadable file or a pulled card are all boundaries the listener
     * either caused or was told about, and softening them would be
     * answering an instruction with a three-second blur. `next` should
     * sound like next.
     */
    {
        const uint32_t sec = settings_crossfade_sec();
        bool ok = (sec > 0) && s_prev_ended_clean;

        /*
         * NOT INTO A TRACK SHORTER THAN THE FADE.
         *
         * The writer clamps the fade to the outgoing tail -- `tail was
         * shorter` -- and nothing clamped it to the INCOMING track. A
         * three-second file under a ten-second fade never reaches full
         * volume and is over before the fade ends, so it is not played
         * so much as passed through: the board ran a 10244 ms fade into
         * a 3 s track and the track was inaudible.
         *
         * Twice the fade is the threshold, so the incoming track gets
         * at least as long at full volume as it spent arriving. The
         * length comes from the sidecar, which track_change_begin()
         * loaded a moment ago; a track with no recorded length is not
         * refused, because unknown is not short.
         */
        if (ok && sec) {
            const uint32_t in_len = (rg_holding(path) && s_rg.format.present)
                                  ? s_rg.format.sec : 0;
            if (in_len && in_len < sec * 2) {
                ok = false;
                ESP_LOGI(TAG, "no crossfade: the incoming track is only "
                              "%" PRIu32 " s", in_len);
            }
        }

        /*
         * Never over itself. Repeat-one, or a `next` that wraps a
         * one-track folder, hands this function the file that is still
         * playing out of the other ring -- and an overlap of a recording
         * with itself three seconds out of phase is not a transition,
         * it is a flanger. The rings cannot tell: same rate, same
         * channels, both full.
         */
        if (ok && strcmp(path, s_prev_path) == 0) {
            ok = false;
            ESP_LOGI(TAG, "no crossfade: same file");
        }

        if (ok && !settings_crossfade_album() && same_album(path, s_prev_dir)) {
            ok = false;
            ESP_LOGI(TAG, "no crossfade: same album");
        }

        s_xfade_ms = sec * 1000u;
        s_xfade_armed = ok;
    }

    /*
     * Anything still fading belongs to a track that ended before this
     * one was chosen. See fade_out_cancel() -- it is a no-op unless a
     * ramp is actually running, so a gapless boundary does not reach
     * the flush inside it.
     *
     * Here rather than at the choice, because this is the point after
     * which a first block can be produced, and a ramp that outlives
     * that would be applied to the wrong track.
     */
    fade_out_cancel();

    /*
     * Loudness is measured off the PCM that is about to be decoded for
     * the speaker anyway -- see loudness.h. Skipped entirely when the
     * sidecar already holds a current measurement, so a track that has
     * been played once costs nothing on every later play.
     *
     * A local, not a static: it belongs to this attempt at this track
     * and nothing else may touch it, which is the same single-owner
     * rule the ring follows.
     */
    static loudness_t s_loud;      /* static only because it is ~4 KB */
    bool measuring = false;
    float rg_scale = 1.0f;         /* linear; 1.0 is unity */
    bool  fmt_saved = false;       /* the format line is written once */

    /* Held, not published. Released by VISUALS_GATE() with everything
     * else the listener is not supposed to see early. */
    bool  rg_pending_active = false;
    bool  rg_pending_measuring = false;

    /*
     * This track's measured loudness, for the crossfade's level match.
     *
     * Kept as a local and copied into the ring's slot alongside the
     * sample rate, because it is a property of the audio being queued
     * and the queue is what the writer reads. Unknown on a first play,
     * which is exactly when there is no measurement yet -- the match
     * falls back to unity and the overlap is unmatched rather than
     * matched by a guess.
     */
    float track_lufs = 0.0f;
    bool  track_lufs_known = false;
    float rg_pending_db = 0.0f;
    {
        /*
         * One load, everything it holds put to use.
         *
         * Before decoder_open() rather than after: the tags and the
         * "no cover" answer are wanted for the first repaint, which
         * happens while the open is still running, and the whole point
         * of storing them was not to wait for a file read to get them.
         *
         * Held since track_change_begin(), so this is the same record
         * the tags and the cover answer already came from -- no third
         * read of one file, and no 3 KB copy of it either.
         */
        const bool got = rg_holding(path);
        const replaygain_t *const have = &s_rg;
        const bool known = got && have->loudness.present;

        /*
         * Read once, here, and not again for the rest of the track.
         *
         * The switch can be thrown from the panel while this loop is
         * running, and a loop that re-read it would apply a gain to the
         * second half of a track it measured the first half of -- which
         * is not "the setting took effect", it is a step in level and a
         * measurement that describes neither version. Sampling it at the
         * top makes the track the unit, which is what the panel promises
         * the listener.
         */
        const bool rg_on = settings_rg_enabled();

        /*
         * MEASURING IS NOT GATED BY THE SWITCH, and 0603 had this wrong.
         *
         * The reasoning there was that measuring and applying are one
         * activity, so one switch should cover both. The premise was
         * false. Measuring is not separate work this player can decline
         * to do: the pass it rides on happens anyway, because the
         * waveform is always drawn and the envelope walk is that pass.
         * Loudness costs two biquads and a running sum over samples that
         * have already been decoded for the picture -- the marginal cost
         * of measuring is the arithmetic, not the read.
         *
         * So switching ReplayGain off to save the measurement saves
         * nothing, and costs the listener the ability to turn it back on
         * and have it work immediately. Off now means one thing: DO NOT
         * APPLY. The sidecar fills in either way, and the day the switch
         * goes back on, the library is already measured.
         */
        measuring = !known;

        if (got) {
            /* Seed the RAM caches so load_tags() and do_art() find
             * their answers without opening anything. Same copying
             * accessors as before -- mediacache.c reserves borrowed
             * pointers for media_task, and this is the decode loop. */
            if (have->waveform.present && have->waveform.columns > 0) {
                framewalk_t w;
                memset(&w, 0, sizeof(w));
                w.has_levels = true;
                w.columns = have->waveform.columns;
                w.sec     = have->waveform.sec;
                memcpy(w.level, have->waveform.level,
                       (size_t)have->waveform.columns);
                mediacache_put_walk(path, &w);
            }
            if (have->tags.present) {
                id3_tags_t t;
                memset(&t, 0, sizeof(t));
                snprintf(t.title,  sizeof(t.title),  "%s", have->tags.title);
                snprintf(t.artist, sizeof(t.artist), "%s", have->tags.artist);
                snprintf(t.album,  sizeof(t.album),  "%s", have->tags.album);
                mediacache_put_tags(path, &t);
            }
            if (have->art.present && !have->art.has_art) {
                mediacache_put_no_art(path);
            }
            /*
             * The duration, before the decoder has found it. For a
             * Xing-less MP3 that is a whole-file index build away, so
             * this is the difference between a seek bar that works from
             * the first repaint and one that appears a second and a
             * half later. decoder_open() overwrites it with its own
             * answer when it has one.
             */
            /*
             * The waveform's sec is the fallback, for every sidecar
             * written before there was a format section to put it in.
             * It is the same number by a worse route -- see
             * rg_note_format() -- and a card full of old sidecars should
             * not lose a working seek bar to a schema change.
             */
            uint32_t sec = have->format.present ? have->format.sec : 0;
            if (!sec && have->waveform.present) sec = have->waveform.sec;
            if (sec) {
                seed_len = sec;
                /* Only onto the screen if the screen is this track's
                 * already. During a decode-ahead the bar belongs to the
                 * song still playing; the gate publishes this one. */
                if (!tail_playing()) s_len_sec = seed_len;
            }
        }

        /*
         * Learned now, shown at the handoff -- like the title, the
         * envelope and the length above it.
         *
         * These three used to be published here, which is where this
         * track's DECODE begins, up to a ring before any of it is
         * heard. On a boundary that is twenty seconds: the indicator
         * switched to the incoming track's gain while the outgoing one
         * was still playing, and the number on screen belonged to audio
         * nobody could hear yet.
         *
         * It was invisible until 0508 and 0509 removed the cyan flash
         * that happened at the same instant. The flash was covering it.
         *
         * rg_scale is NOT deferred and must not be: it is applied to the
         * samples this loop is decoding, which are the incoming track's,
         * and holding it back would play the first twenty seconds of
         * every track at the wrong level. The gain applies to the audio
         * when the audio is made; the indicator describes the audio when
         * it is heard. Same number, two different moments, and that is
         * the whole of this bug.
         */
        /*
         * Taken from the record rather than from any of the branches
         * below, because it is wanted in all of them: whether the gain
         * is being applied or not, the number describes this file.
         */
        if (known && have->loudness.present) {
            track_lufs = have->loudness.integrated_lufs;
            track_lufs_known = true;
        }

        rg_pending_measuring = measuring;
        if (!rg_on) {
            /*
             * Off: measured, filed, not applied, not shown.
             *
             * rg_scale stays at unity and rg_pending_active stays false,
             * so the indicator does not appear -- which matters more than
             * it sounds. The mark on the bar is a claim that the level
             * being heard is not the file's own, and drawing it while the
             * feature is off would be that claim made falsely.
             *
             * The measurement still runs (see `measuring` above) and
             * still reaches the sidecar, so the library gets measured as
             * it is played whether or not anybody is listening to the
             * result. That is the point: the switch changes what comes
             * out of the speaker, not what the player learns.
             */
            if (measuring) loudness_reset(&s_loud);
            rg_pending_active = false;
            rg_pending_db = 0.0f;
        } else if (measuring) {
            loudness_reset(&s_loud);
            /*
             * Measuring and applying are mutually exclusive, and have to
             * be: the measurement has to see the file's own level, so a
             * pass that applied a gain would measure the gain back and
             * converge on the reference no matter what the track is.
             * The first play measures at unity, every later one applies.
             */
            rg_pending_active = false;
            rg_pending_db = 0.0f;
        } else {
            const float g = replaygain_gain_db(&have->loudness);
            rg_scale = powf(10.0f, g / 20.0f);
            /*
             * EFFECTIVE loudness, not measured: the gain has been folded
             * into the samples going into the ring, so what the writer
             * will play is the file's level plus g -- the reference, by
             * construction. Publishing the raw figure here would have
             * the crossfade match two tracks that are already matched,
             * and mismatch them in the process.
             *
             * This is also what makes the half-measured case right. With
             * ReplayGain on and one of the two tracks never played
             * before, one ring is at the reference and the other is at
             * whatever it was mastered at, and the overlap between them
             * is the one place that difference is audible. Comparing
             * effective levels sees that; comparing measured ones, or
             * skipping the match whenever ReplayGain is on, does not.
             */
            track_lufs = have->loudness.integrated_lufs + g;
            track_lufs_known = true;
            rg_pending_active = true;
            rg_pending_db = g;
            ESP_LOGI(TAG, "replaygain: %.2f LUFS, peak %.2f dBFS -> %+.2f dB",
                     (double)have->loudness.integrated_lufs,
                     (double)have->loudness.sample_peak_dbfs, (double)g);
        }
    }

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

        /* Nothing was decoded, but the record may still hold tags and
         * the cover answer from the open above, so it is released the
         * same way the normal exit releases it. */
        rg_release();

        /*
         * Guarded exactly as the normal exit is, and for the same
         * reason. This path runs on the INCOMING track -- it is reached
         * from play_file() for the track that could not allocate -- and
         * at a boundary that is a ring before the OUTGOING track's last
         * sample is heard. Clearing here would take the previous song's
         * ReplayGain mark off the screen twenty seconds early, which is
         * the bug 0511 fixed on the path next to this one.
         *
         * Missed there because it is the out-of-memory path and nobody
         * looks at it, which is what the comment a few lines up says
         * about s_decoding for the same path. Two things now depend on
         * this branch being written as carefully as the ordinary one.
         */
        if (!tail_playing()) {
            s_rg_measuring = false;
            s_rg_active = false;
            s_rg_gain_db = 0.0f;
        }
        return TRACK_UNREADABLE;
    }

    uint32_t cur_rate = 0;
    int cur_chans = 0;
    int blocks = 0;
    uint64_t frames_out = 0;
    track_end_t why = TRACK_ENDED;
    bool decode_failed = false;

    /*
     * Where the last serviced seek left the frame counter, and whether
     * there was one.
     *
     * Only used to answer one question at the bottom of this function:
     * did the track end because it reached its end, or because a seek
     * put the decoder within a moment of it. See s_prev_ended_clean.
     */
    uint64_t frames_at_seek = 0;
    bool     seeked = false;

    /*
     * A time-to-offset table being recorded as this track plays, for
     * the one kind of stream that has no other source of one: raw ADTS
     * that declares itself VBR. See decoder_needs_table().
     *
     * The pair is (where the decoder's next byte is, how many frames
     * have come out), sampled every REPLAYGAIN_INDEX_SPACING_SEC, and
     * it costs an ftell() at that rate. Nothing is read, nothing is
     * scanned: the play was happening anyway, which is the same bargain
     * as the loudness, the envelope and 0703's MP3 table.
     */
    uint32_t tbl_off[REPLAYGAIN_INDEX_MAX];
    uint32_t tbl_frm[REPLAYGAIN_INDEX_MAX];
    int      tbl_n = 0;
    uint32_t tbl_spacing = REPLAYGAIN_INDEX_SPACING_SEC;
    uint64_t tbl_next = 0;
    /* Only when the decoder says nothing else can seek this stream. */
    bool     tbl_rec = decoder_needs_table(dec);
    if (tbl_rec) {
        ESP_LOGI(TAG, "adts: no seek mechanism; recording a table as it plays");
    }

    /*
     * The screen changes when the sound does, not when the decode does.
     *
     * With the drain gone, up to a ring of the previous track is still
     * to be played when this call starts filling the other one. Drawing
     * this track's title, envelope and position now would put them on
     * screen up to twenty seconds before anything of this track is
     * audible -- and would reset the position of a track that is still
     * playing.
     *
     * So both are deferred to the moment the writer arrives on this
     * ring, which is the moment this track becomes the one being heard.
     * The common case has no wait in it at all: the previous ring is
     * usually already empty, the writer has already moved, and the gate
     * below passes on its first look.
     */
    bool visuals_pending = true;

    /*
     * The handoff, from this side. s_ring_play catches up when the
     * previous track's ring runs dry; that is this track's first
     * audible sample, so it is where its name and its bar belong.
     *
     * A macro, and checked in two places, because of where the decode
     * loop actually spends its time once it is running ahead. With
     * twenty seconds of the previous track still queued, this track
     * fills its ring and parks in xStreamBufferSend() -- 15 s in one
     * call, measured -- so a gate at the top of the loop does not run
     * again until long after the handoff it is watching for:
     *
     *   I (357988) first sound 2194 ms after the press, ring 1%
     *   W (376296) ring send blocked 15135 ms (ring 99%)
     *   I (376297) tags from cache: "Handel..."
     *
     * Eighteen seconds of the previous track's title and a seek bar
     * that had not moved. The send is already sliced at SEND_SLICE_MS
     * so the transport stays responsive; this rides the same slices.
     */
#define VISUALS_GATE()                                                  \
    do {                                                                \
        if (visuals_pending && cur_rate != 0 && !s_tail_pending) {      \
            /* One condition, the same one track_change_begin() used,   \
             * so the deferral and its release cannot disagree. The     \
             * index comparison this replaced was a second way of       \
             * asking, and two ways of asking one question is how the   \
             * screen ended up ahead of the sound in the first place.   \
             * cur_rate is still required: the bar needs this track's   \
             * length and that is not known until the open returns. */  \
            visuals_pending = false;                                    \
            s_len_sec = len_sec;                                        \
            s_can_seek = can_seek;                                      \
            s_stats_valid = true;                                       \
            /* The ReplayGain indicator, with the rest of the screen.   \
             * See where these are computed for why the gain itself is  \
             * not deferred with them. */                               \
            s_rg_active = rg_pending_active;                            \
            s_rg_measuring = rg_pending_measuring;                      \
            s_rg_gain_db = rg_pending_db;                               \
            settings_set_track(path);                                   \
            /* The chooser's playing marker, with the title and the bar. \
             * It used to work this out from playlist_current(), which  \
             * is where the decoder is -- a ring ahead of the speaker.   \
             * See browser_set_playing(). */                            \
            browser_set_playing(path);                                  \
            track_change_show();                                        \
            load_track_visuals(path);                                   \
        }                                                               \
    } while (0)

    /* Consume any repaint the chooser left pending. It closed a moment
     * ago and set the flag, and the visuals gate above will draw this
     * track's cover when it becomes the audible one, so the flag is
     * already spoken for.
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

    /*
     * Learned now, shown at the handoff.
     *
     * These used to be published here, which is where the decode of this
     * track starts -- up to a ring before any of it is audible. The bar
     * snapped to this track's length and back to zero while the previous
     * track was still playing, and then had nothing to say for twenty
     * seconds. The title and the envelope were already deferred to the
     * gate for exactly this reason; the numbers behind the bar belong
     * there with them.
     *
     * Kept as locals as well because the decode loop needs them itself
     * -- the seek target, the format record, the envelope's duration --
     * and during a decode-ahead the published values are still the
     * previous track's and would be the wrong answer to all three.
     */
    bool can_seek = decoder_can_seek(dec);
    uint32_t len_sec = decoder_duration_sec(dec);

    /*
     * Whether the CONTAINER answered, before the sidecar's copy is
     * allowed to stand in for it.
     *
     * The distinction is the whole of what gets filed later. A length
     * that came out of a header costs nothing to find again next time
     * and is not worth a sidecar section; one that had to be walked for
     * is. Testing len_sec after the seed has been folded in cannot tell
     * those apart -- the seed IS the sidecar, so every track would look
     * like it already knew.
     */
    const bool len_from_header = (len_sec != 0);

    if (!len_sec) len_sec = seed_len;
    if (len_sec == 0) {
        ESP_LOGI(TAG, "no duration available; seek bar will stay empty");
    }

    /*
     * For the bitrate, when the backend will not give one -- which is
     * every esp_audio_codec format. Taken once, here, because the file
     * is open and cannot change under us, and because stat() on a USB
     * drive mid-decode is a read the arbiter would have to schedule.
     */
    uint64_t file_bytes = 0;
    {
        struct stat st;
        if (stat(path, &st) == 0 && st.st_size > 0) {
            file_bytes = (uint64_t)st.st_size;
        }
    }

    /*
     * The last info the decoder handed back, outside the loop.
     *
     * decoder_read() fills a fresh one per call and it goes out of scope
     * with the iteration, which is fine for the per-block work but not
     * for the two places that file a format section -- one of them runs
     * before the read, and the other runs after the loop has ended.
     * Zeroed, so a track that never produced a block files nothing
     * rather than filing whatever was on the stack.
     */
    decoder_info_t track_info;
    memset(&track_info, 0, sizeof(track_info));

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
        if (s_repaint_art && !s_pending_ready && !visuals_pending) {
            s_repaint_art = false;
            load_track_visuals(path);
        }

        /* The envelope landed. Drawn here rather than on the loading
         * task so there is one writer to the framebuffer.
         *
         * Checked against the path first: media_task may have prepared
         * this for a track that has since been skipped past, and the
         * flag alone cannot tell. */
        if (s_wave_ready && strcmp(s_wave_path, path) == 0 && !visuals_pending) {
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
            wave_show(&s_walk, path);

            /* The walk counted frames on the way past. For a format that
             * states its own length this is redundant and is not used;
             * for raw ADTS, AMR and a Xing-less MP3 it is the only answer
             * there is, and the bar goes from empty to filled mid-track
             * rather than staying empty for the whole song. */
            if (!len_sec && s_walk.sec) {
                len_sec = s_walk.sec;
                /* Mirrored only once the bar is this track's, for the
                 * same reason it is not published above. */
                if (!visuals_pending) s_len_sec = len_sec;
                ESP_LOGI(TAG, "duration from frame walk: %" PRIu32 "s",
                         len_sec);
                /* Filed the moment it is known rather than at the track
                 * end, because a walk that finished is a number already
                 * paid for and a track can be skipped out of before its
                 * boundary ever runs. */
                rg_note_format(path, &track_info, len_sec, file_bytes);
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
            /*
             * Before the seek is applied, and regardless of whether it
             * turns out to be possible. BS.1770 integrates over the
             * whole programme, so a measurement that skipped a section
             * is not a slightly worse number -- it is a number about
             * different audio, and nothing downstream could tell. A
             * refused seek still says the listener stopped listening
             * straight through.
             */
            if (measuring) {
                loudness_invalidate(&s_loud);
                measuring = false;
                s_rg_measuring = false;
                ESP_LOGI(TAG, "loudness measurement dropped: seek");
                /* Counted, so a track that is always seeked through is
                 * distinguishable from one never played. */
                if (rg_holding(path)) {
                    s_rg.attempts.present = true;
                    s_rg.attempts.abandoned++;
                    s_rg_dirty = true;
                }
            }

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

            if (len_sec == 0) {
                ESP_LOGI(TAG, "seek ignored: no duration for this format");
            } else {
                const uint32_t target = (uint32_t)((uint64_t)len_sec * pct / 100);
#if SEEK_NOOP
                /* See SEEK_NOOP. Everything below runs as if the seek
                 * had succeeded; the decoder is simply never asked. */
                const esp_err_t sr = ESP_OK;
                uint32_t landed = target;
                ESP_LOGW(TAG, "SEEK_NOOP: not seeking to %" PRIu32 "s", target);
#else
                uint32_t landed = target;
                const esp_err_t sr = decoder_seek_sec_at(dec, target, &landed);
#endif
                if (sr == ESP_ERR_NOT_SUPPORTED) {
                    ESP_LOGI(TAG, "seek ignored: %s cannot seek", "this backend");
                } else if (sr != ESP_OK) {
                    ESP_LOGW(TAG, "seek to %" PRIu32 "s failed", target);
                } else {
                    /* Drop what is already queued. Without this the ring
                     * plays out ~0.37 s of the old position after the
                     * jump, which sounds like the seek was ignored and
                     * then took effect late. */
#if !SEEK_NOOP && !SEEK_KEEP_RING
                    /* Ask the writer to drop what is queued. It owns the
                     * read side; this task must not touch it. See
                     * s_pcm_flush. */
                    s_flush_why = "seek";
                    s_pcm_flush = true;

                    /* The ring is about to be empty and nothing is
                     * draining it, so the refill below would otherwise
                     * run flat out. See REFILL_DECODE_SPEEDUP. */
                    s_refill_pacing = true;
#elif SEEK_KEEP_RING
                    /* See SEEK_KEEP_RING. The decoder has moved; the
                     * queued audio has not, so the jump arrives when the
                     * ring drains to it. */
                    ESP_LOGW(TAG, "SEEK_KEEP_RING: ring not reset");
#endif

                    /* 0505: capture the pack across the commit. Armed
                     * here rather than at the press because this is the
                     * instant the flash follows -- the release only
                     * records a target, and the decode loop is where it
                     * is acted on. */
                    battery_trace_arm("seek");

                    /*
                     * Re-anchor the position counter, or the clock
                     * counts on from where it was rather than from the
                     * new point.
                     *
                     * Anchored to where the decoder LANDED, not to what
                     * was asked for. They are the same on minimp3,
                     * which decodes forward to the exact sample, and
                     * they are not on the FLAC bisection, which lands
                     * on the frame containing the target -- up to 93 ms
                     * before it at a 4096-sample block. Anchoring to
                     * the request would make the clock disagree with
                     * the audio by that much for the rest of the track,
                     * which is a small error that never corrects
                     * itself.
                     */
                    frames_out = (uint64_t)landed * (cur_rate ? cur_rate : 1);
                    s_frames_out[s_ring_fill] = frames_out;
                    frames_at_seek = frames_out;
                    seeked = true;
                    s_pos_sec = landed;
                    if (landed != target) {
                        ESP_LOGI(TAG, "seek to %" PRIu32 "s (landed %" PRIu32 "s)",
                                 target, landed);
                    } else {
                        ESP_LOGI(TAG, "seek to %" PRIu32 "s", target);
                    }
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
        /* Checked here and inside the send below; see the note on
         * VISUALS_GATE. */
        VISUALS_GATE();

        const TickType_t t_read = xTaskGetTickCount();

        decoder_info_t info;
        const int n = decoder_read(dec, pcm, DECODER_MAX_INT16, &info);
        /*
         * A FAILED DECODE IS NOT THE END OF THE TRACK, AND THE
         * DIFFERENCE IS WHAT GETS WRITTEN TO THE CARD.
         *
         * These were one test, so a stream that errored on its first
         * frame looked exactly like one that ran out of file. File 15
         * produced a single block, failed, and was still counted as a
         * complete uninterrupted play: -46.16 LUFS off zero gated
         * blocks and a one-column envelope for a sixty second track,
         * written to a sidecar and loaded back on the next play. A
         * measurement nothing can later tell apart from a real one is
         * the exact thing the write rules exist to prevent.
         */
        if (n < 0) {
            decode_failed = true;
            break;
        }
        if (n == 0) break;
        track_info = info;

        const uint32_t read_ms =
            (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount() - t_read);
        if (read_ms > LOOP_STALL_MS) {
            ESP_LOGW(TAG, "decoder_read blocked %" PRIu32 " ms", read_ms);
        }

        /* The whole cost of measuring: two biquads and a running sum
         * per sample, over a buffer that has already been decoded. */
        if (measuring) {
            loudness_process(&s_loud, pcm, n, info.channels,
                             (uint32_t)info.sample_rate);
        }

        /*
         * Applied here rather than in audio_out.c because only the USB
         * route has a software gain stage -- the analog path sets the
         * ES8388's own registers, and a per-route gain would mean the
         * same track played at two different levels depending on what
         * is plugged in. This is one pass over a block that is already
         * in cache from the decode.
         *
         * Never runs while measuring: see the note where rg_scale is
         * computed.
         */
        if (rg_scale != 1.0f) {
            for (int i = 0; i < n; i++) {
                int v = (int)lrintf((float)pcm[i] * rg_scale);
                if (v >  32767) v =  32767;
                if (v < -32768) v = -32768;
                pcm[i] = (int16_t)v;
            }
        }

        if ((uint32_t)info.sample_rate != cur_rate || info.channels != cur_chans) {
            /*
             * The trim state, on the line that already describes the
             * format, because that is where you look when a boundary
             * sounds wrong.
             *
             * Nothing acts on it yet and nothing should: a hard cut at a
             * slightly wrong sample is the behaviour this player has
             * always had, and changing it is not what this patch is. It
             * is here so that playing a library through and reading the
             * log answers the question a crossfade has to ask before it
             * can be built -- how much of this collection is actually
             * sample-exact. If every track says "exact", the overlap can
             * be unconditional. If half say "unknown", that is worth
             * knowing before the overlap is written on top of it rather
             * than after.
             *
             * See decoder_trim_t for why "unknown" and "untrimmed" are
             * different answers.
             */
            ESP_LOGI(TAG, "%s: %d Hz, %d ch, %d kbps, trim %s",
                     info.codec, info.sample_rate, info.channels,
                     info.bitrate_kbps, decoder_trim_name(info.trim));

            /* For the format card, which is what a file with no picture
             * in it shows instead of a cover. Published rather than
             * fetched: media_task cannot ask the decoder anything -- the
             * decoder_t belongs to this loop, and handing it across is
             * the mistake s_ring_pct exists to avoid. */
            snprintf(s_fmt_codec, sizeof(s_fmt_codec), "%s",
                     info.codec ? info.codec : "");
            s_fmt_rate = info.sample_rate;
            s_fmt_chans = info.channels;
            /*
             * The backend's figure, or the file over its duration when
             * there is not one -- which is every esp_audio_codec format.
             * The card was drawing "0 kbps" for those, and a zero on a
             * line of real measurements reads as a measurement.
             *
             * Only when both numbers are in: with no duration there is
             * nothing to divide by, and zero stays, which is at least
             * the honest form of not knowing.
             */
            s_fmt_kbps = info.bitrate_kbps;
            if (s_fmt_kbps <= 0 && file_bytes && len_sec) {
                s_fmt_kbps = (int)(((file_bytes * 8) / len_sec + 500) / 1000);
            }
            s_fmt_known = true;

            /*
             * To the card, once. Everything here is what the decoder
             * reports after the open, so a listing can show it without
             * opening anything -- and the duration especially, which
             * for a Xing-less MP3 costs a whole-file index build to
             * learn. `fmt_saved` keeps this to the first block rather
             * than every one.
             */
            if (!fmt_saved && rg_holding(path)) {
                fmt_saved = true;
                replaygain_format_t rf;
                memset(&rf, 0, sizeof(rf));
                rf.present = true;
                rf.sec = len_sec;
                rf.sample_rate = (uint32_t)info.sample_rate;
                rf.channels = (uint8_t)info.channels;
                rf.kbps = (uint16_t)info.bitrate_kbps;
                snprintf(rf.codec, sizeof(rf.codec), "%s",
                         info.codec ? info.codec : "");
                /* Only when it actually differs, so a track whose
                 * format was already recorded does not mark the record
                 * dirty and rewrite an identical file every play. */
                if (memcmp(&s_rg.format, &rf, sizeof(rf)) != 0) {
                    s_rg.format = rf;
                    s_rg_dirty = true;
                }
            }

            if (cur_rate == 0) {
                /*
                 * A rate this output stage cannot take is a property of
                 * the file, so it is reported like any other unplayable
                 * file. It used to be ESP_ERROR_CHECK'd, which turned a
                 * 96 kHz WAV in a folder into a reboot loop: the track
                 * is chosen, the player panics, it comes back, and the
                 * restored track is the one that panics.
                 */
                if (!audio_out_rate_supported((uint32_t)info.sample_rate)) {
                    ESP_LOGW(TAG, "%d Hz is above what this output can clock; skipping",
                             info.sample_rate);
                    why = TRACK_UNREADABLE;
                    break;
                }

                /*
                 * The reconfigure disables the I2S channel, so it must
                 * not happen with the previous track still playing out
                 * of the other ring -- which, without the drain, is the
                 * normal state at a boundary. Wait for the writer to
                 * arrive before touching the clock.
                 *
                 * Only when the rate actually changes. An album at one
                 * rate never waits, which is every album; this costs
                 * the gapless property exactly on the boundaries where
                 * the hardware cannot have it anyway.
                 */
                /*
                 * THE CROSSFADE DECISION IS ABOUT THE RATE, NOT ABOUT
                 * THE DRAIN.
                 *
                 * 0710 put the disarm inside the drain below, which
                 * only runs when the rings have not yet converged --
                 * so a rate change at a boundary where the previous
                 * track had already finished playing out left the fade
                 * armed and said nothing. The writer then refused it
                 * silently, on rates that cannot be mixed, and the log
                 * showed a crossfade that simply never happened.
                 *
                 * The board found it on a 44.1 kHz Vorbis file either
                 * side of a 48 kHz Opus one: both boundaries lost their
                 * fade, and it read as Ogg misbehaving rather than as
                 * two files that cannot be overlapped.
                 */
                if ((uint32_t)info.sample_rate != audio_out_rate() &&
                    (s_xfade_armed || s_xfade_active)) {
                    ESP_LOGI(TAG, "no crossfade: %" PRIu32 " Hz into %d Hz",
                             audio_out_rate(), info.sample_rate);
                    s_xfade_armed = false;
                    s_xfade_active = false;

                    /*
                     * A DIP INSTEAD, IN TWO HALVES.
                     *
                     * Half the configured crossfade ramping the
                     * outgoing track to silence, then the drain and the
                     * reconfigure, then the other half ramping this one
                     * up. The listener asked for N seconds of softening
                     * and gets N seconds of it; what changes is that
                     * the two tracks never sound at once, which is the
                     * only thing the hardware forbids.
                     *
                     * The down ramp is armed against the OUTGOING rate,
                     * because it is measured in frames of a ring that
                     * is still being clocked at the old rate. Using the
                     * new one would make it wrong by the ratio of the
                     * two -- 9% for 44.1 into 48, which is what this
                     * whole path exists because of.
                     */
                    const uint32_t half_ms = settings_crossfade_sec() * 500u;
                    if (half_ms) {
                        const uint32_t out_rate = audio_out_rate()
                                                ? audio_out_rate() : 44100;
                        s_dip_dir = 0;
                        s_dip_pos = 0;
                        s_dip_arm = (uint32_t)((uint64_t)out_rate *
                                               half_ms / 1000u);
                        s_dip_in_frames = (uint32_t)
                            ((uint64_t)info.sample_rate * half_ms / 1000u);
                        ESP_LOGI(TAG, "rate change: dipping %" PRIu32
                                      " ms down, %" PRIu32 " ms up",
                                 half_ms, half_ms);
                    }
                }

                /*
                 * THE DRAIN ASKED THE WRONG QUESTION.
                 *
                 * It was `s_ring_play != s_ring_fill` -- the indices
                 * differing -- but the ring switch happens BELOW this,
                 * after the reconfigure. At an ordinary boundary the
                 * previous track's ring is still both `play` and
                 * `fill`, so the test was false, the drain was skipped,
                 * and audio_out_set_format() reconfigured the I2S clock
                 * with up to twenty seconds of the outgoing track still
                 * queued in the ring the writer was reading.
                 *
                 * That audio does not play. The board logged `tail:
                 * 3516 KB of this track still to play` and then `the
                 * finished track has played out` 111 ms later: twenty
                 * seconds of Vorbis discarded at a rate change, heard
                 * as the track jumping to its end.
                 *
                 * It ever worked at all by accident -- the indices
                 * happen to differ when the PREVIOUS boundary left them
                 * that way, which is why this only showed on some
                 * transitions.
                 *
                 * The real question is whether anything is still queued
                 * for the writer, which is one thing and is directly
                 * observable. The dip armed above then has something to
                 * ramp, and the reconfigure happens after silence
                 * rather than through the middle of a track.
                 */
                const bool tail_queued =
                    s_pcm &&
                    xStreamBufferBytesAvailable(s_ring[s_ring_play]) > 0;

                if ((uint32_t)info.sample_rate != audio_out_rate() &&
                    tail_queued) {
                    ESP_LOGI(TAG, "rate change to %d Hz; draining %u KB first",
                             info.sample_rate,
                             (unsigned)(xStreamBufferBytesAvailable(
                                 s_ring[s_ring_play]) / 1024));
                    /*
                     * AND THE CROSSFADE IS OFF FOR THIS BOUNDARY.
                     *
                     * The two cannot both happen. A crossfade mixes the
                     * two rings, which only means anything if they hold
                     * the same sample rate; the drain below waits for
                     * the outgoing ring to empty, and the writer's fade
                     * is what keeps it from emptying.
                     *
                     * Left armed, they wait for each other: the board
                     * logged a rate change into a 12 s fade taking
                     * 17.8 s to make a sound, ending in `crossfade cut
                     * short: the outgoing ring emptied`. The fade was
                     * mixing against a ring the decode loop could not
                     * fill, because the decode loop was in this wait.
                     *
                     * Disarming here rather than at the arming block
                     * because that is the earliest the incoming rate is
                     * known: it comes from the first decoded frame,
                     * which is after the decision to crossfade was
                     * made.
                     */
                    /* Belt and braces: the test above has already
                     * disarmed on rate, so this only fires if something
                     * armed one in between. Kept because the cost of
                     * the fade and the drain waiting for each other is
                     * seventeen seconds of silence. */
                    if (s_xfade_armed || s_xfade_active) {
                        ESP_LOGI(TAG, "no crossfade: the rate changes");
                        s_xfade_armed = false;
                        /*
                         * The active flag as well as the armed one.
                         * 0710 cleared only `armed`, which stops a fade
                         * that has not begun -- and the board then ran
                         * one that had, mixing against a ring the
                         * decode loop could not refill and logging
                         * `crossfade cut short: the outgoing ring
                         * emptied` nine seconds later. A fade already
                         * under way is exactly the case this needs to
                         * stop.
                         */
                        s_xfade_active = false;
                    }
                    /*
                     * Until the ring is empty, not until the indices
                     * agree. A press still cuts it short: a listener
                     * who has asked for something else has already said
                     * what the tail is worth.
                     */
                    while (s_pcm &&
                           xStreamBufferBytesAvailable(s_ring[s_ring_play]) > 0) {
                        if (s_pending_ready || s_seek_pct >= 0) break;
                        vTaskDelay(pdMS_TO_TICKS(10));
                    }
                }

                const esp_err_t ferr =
                    audio_out_set_format((uint32_t)info.sample_rate, 2);
                /*
                 * The up half, started here rather than by the writer's
                 * countdown: there is nothing to count down: the ring
                 * is empty, the clock has just been reconfigured, and
                 * the very next chunk is the first of the new track.
                 */
                /*
                 * And the down ramp's arming is cleared whether it ever
                 * fired or not. If the outgoing ring was already empty
                 * when the dip was set up there was nothing to fade,
                 * and an arm left standing would ambush the INCOMING
                 * track: its ring falls past the same threshold near
                 * its own end, and it would fade out in the middle of
                 * a track nobody asked to end.
                 */
                s_dip_arm = 0;
                if (ferr == ESP_OK && s_dip_in_frames) {
                    s_dip_frames = s_dip_in_frames;
                    s_dip_pos = 0;
                    s_dip_dir = 1;
                    s_dip_in_frames = 0;
                } else {
                    s_dip_in_frames = 0;
                    s_dip_dir = 0;
                }
                if (ferr != ESP_OK) {
                    ESP_LOGW(TAG, "cannot play %d Hz (%s); skipping",
                             info.sample_rate, esp_err_to_name(ferr));
                    why = TRACK_UNREADABLE;
                    break;
                }
                /*
                 * The ring and the writer are made once in app_main()
                 * now, so a track start sets the rate and publishes an
                 * occupancy and that is all. A boot with no ring is
                 * refused here rather than hanging silently, same as
                 * before.
                 */
                if (!s_pcm) {
                    ESP_LOGE(TAG, "no PCM ring; cannot play");
                    why = TRACK_UNREADABLE;
                    break;
                }

                /*
                 * The other ring, always.
                 *
                 * The writer is on the one the previous track used and
                 * moves across when it runs dry, so this track's first
                 * samples go somewhere the writer is not reading -- the
                 * property gapless needs, established here while the
                 * drain below still guarantees the old ring is already
                 * empty when we arrive.
                 *
                 * Reset rather than trusted: an interrupted track left
                 * its tail in this ring two tracks ago, and the writer
                 * has long since moved off it.
                 *
                 * This one stays a reset, and the sentence above is why:
                 * the writer receives from s_ring[s_ring_play] and this
                 * is the ring about to become s_ring_fill, so no task is
                 * blocked on it. The two resets that were dangerous were
                 * on the ring the writer was reading -- both now go
                 * through s_pcm_flush. If the drain below is ever
                 * removed, this assumption goes with it.
                 *
                 * 0509 makes it conditional, because the boundary still
                 * flashes and this is the only reset left on that path.
                 *
                 * It cannot use s_pcm_flush: that drains BOTH rings, and
                 * at a boundary one of them is the outgoing track's tail
                 * with up to twenty seconds still to be heard. Cutting
                 * that off to test a theory would replace a one-frame
                 * flash with a truncated track.
                 *
                 * So: reset only when there is something to reset, and
                 * say so. The claim above is that this ring is always
                 * already empty here. If that holds, the log stays
                 * silent, no reset happens on any boundary, and a flash
                 * that persists anyway clears xStreamBufferReset() of
                 * the boundary case entirely -- leaving the next track's
                 * open, which is where the T-20 timing pointed from the
                 * very first note.
                 *
                 * If the log is NOT silent, the assumption was wrong,
                 * the writer's relationship to this ring needs
                 * establishing rather than asserting, and that is the
                 * bug.
                 */
                s_ring_fill = (s_ring_fill + 1) % PCM_RINGS;
                s_pcm = s_ring[s_ring_fill];
                if (!xStreamBufferIsEmpty(s_pcm)) {
                    const size_t left = xStreamBufferBytesAvailable(s_pcm);
                    ESP_LOGW(TAG, "incoming ring %d held %u KB; resetting",
                             s_ring_fill, (unsigned)(left / 1024));
                    xStreamBufferReset(s_pcm);
                }

                /* The counters describe the ring, so they are cleared
                 * with it and at the same moment. Rate last and zero
                 * first: a zero rate is what tells pos_publish() this
                 * ring has nothing to report yet, so it must be visible
                 * before any frame count of this track is. */
                s_frames_rate[s_ring_fill] = 0;
                s_frames_out[s_ring_fill] = 0;

                s_ring_pct = 0;
                s_writer_stop = false;
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
        const bool ahead_before = (s_ring_play != s_ring_fill) ||
                                  s_tail_pending || !s_playing;
        const uint32_t pause_epoch_before = s_pause_epoch;
        while (remain) {
            if (s_seek_pct >= 0 || s_pending_ready) break;
            const size_t sent = xStreamBufferSend(s_pcm, src, remain,
                                                  pdMS_TO_TICKS(SEND_SLICE_MS));
            src += sent;
            remain -= sent;
            /*
             * Counted here, as the ring accepts it, and not after the
             * block is through.
             *
             * The position is decoded-minus-queued and the two halves
             * are written by different tasks. Bumping the decoded count
             * after the send left a window -- the whole of the send, up
             * to SEND_SLICE_MS of it -- in which the block was already
             * in the ring and not yet in the count, and any pos_publish()
             * landing in that window subtracted a block that had not
             * been added. The writer publishes every 23 ms, so it landed
             * there often: the second counter and the slider stepped
             * back and forth over one second for as long as a track
             * played.
             *
             * Incrementing per slice keeps the pair consistent at every
             * instant either can be read. It also makes an abandoned
             * send -- a seek or a track change mid-block -- count what
             * was actually queued rather than the whole block.
             */
            frames_out += sent / PCM_BYTES_PER_FRAME;

            /*
             * One pair every `tbl_spacing` seconds of OUTPUT, not of
             * wall clock -- the decode runs ahead of the audio and a
             * table keyed to when the sampling happened would be keyed
             * to nothing.
             *
             * A seek ends the recording rather than corrupting it: from
             * that point the frame count no longer counts this file
             * from its start, so every later pair would be wrong by the
             * size of the jump.
             */
            if (tbl_rec && cur_rate) {
                if (seeked) {
                    tbl_rec = false;
                    tbl_n = 0;
                } else if (frames_out >= tbl_next) {
                    if (tbl_n >= REPLAYGAIN_INDEX_MAX) {
                        /* Full: keep every other pair and double the
                         * spacing, the same way the envelope's columns
                         * merge. A long file ends up coarser rather
                         * than truncated half way through. */
                        for (int k = 1; k * 2 < tbl_n; k++) {
                            tbl_off[k] = tbl_off[k * 2];
                            tbl_frm[k] = tbl_frm[k * 2];
                        }
                        tbl_n = (tbl_n + 1) / 2;
                        tbl_spacing *= 2;
                    }
                    /*
                     * NO UINT32_MAX BOUND HERE, BECAUSE `long` IS 32
                     * BITS ON THIS TARGET.
                     *
                     * The check was `at <= (long)UINT32_MAX`, and that
                     * cast is -1 on a 32-bit long -- so the test was
                     * `at <= -1`, false for every real offset, and not
                     * one pair was ever recorded. Fifteen complete
                     * plays of the file logged `recording a table as it
                     * plays` and stored nothing, silently, because a
                     * table of zero entries fails the `> 1` test at the
                     * other end and says nothing about why.
                     *
                     * A `long` cannot exceed UINT32_MAX here, so the
                     * bound was never a check on this target; it was a
                     * habit from writing the same guard in mp4seek.c,
                     * where the value really is 64-bit and the
                     * comparison really is needed.
                     */
                    const long at = decoder_stream_pos(dec);
                    if (at >= 0 &&
                        (tbl_n == 0 || (uint32_t)at > tbl_off[tbl_n - 1]) &&
                        tbl_n < REPLAYGAIN_INDEX_MAX) {
                        tbl_off[tbl_n] = (uint32_t)at;
                        tbl_frm[tbl_n] = (uint32_t)frames_out;
                        tbl_n++;
                    }
                    tbl_next = frames_out + (uint64_t)tbl_spacing * cur_rate;
                }
            }
            s_frames_rate[s_ring_fill] = cur_rate;
            s_frames_out[s_ring_fill] = frames_out;
            /* Alongside the rate, and for the same reason: it describes
             * the audio in this ring, and the writer needs it per ring
             * to match two tracks against each other. See s_ring_lufs. */
            s_ring_lufs[s_ring_fill] = track_lufs;
            s_ring_lufs_known[s_ring_fill] = track_lufs_known;
            /* The handoff can happen while this call is parked on a
             * full ring, which with decode-ahead is most of a track. */
            VISUALS_GATE();
        }
        ring_publish();

        /*
         * Pace the decode-ahead; see TAIL_DECODE_SPEEDUP.
         *
         * After the send rather than before it, so a block already
         * written to the ring is never held back, and computed from the
         * block just queued so it follows the sample rate rather than
         * assuming one. The sleep is at most a few milliseconds and the
         * control checks are at the top of the loop, so this costs the
         * transport nothing it can feel.
         */
        /*
         * Two reasons to pace, one sleep. The tail case is a decode-ahead
         * running against a track that is still playing; the refill case
         * is a ring emptied by a seek. Both are the same failure -- a
         * decode loop with nothing to wait for -- and both take the same
         * divisor.
         */
        if (s_refill_pacing) {
            const size_t have = s_pcm ? xStreamBufferBytesAvailable(s_pcm) : 0;
            if (have >= (size_t)PCM_RING_BYTES * REFILL_PACE_UNTIL_PCT / 100) {
                s_refill_pacing = false;
                ESP_LOGI(TAG, "refill paced to %d%%; running free",
                         REFILL_PACE_UNTIL_PCT);
            }
        }

        if ((s_tail_pending || s_refill_pacing) && cur_rate) {
            const uint32_t block_ms =
                (uint32_t)((uint64_t)(n / (info.channels > 0 ? info.channels : 1))
                           * 1000 / cur_rate);
            const uint32_t div = s_tail_pending ? TAIL_DECODE_SPEEDUP
                                                : REFILL_DECODE_SPEEDUP;
            const uint32_t pace_ms = block_ms / div;
            if (pace_ms) vTaskDelay(pdMS_TO_TICKS(pace_ms));
        }

        const uint32_t send_ms =
            (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount() - t_send);
        /*
         * A full ring is a stall only if somebody is waiting for the
         * audio. While the writer is still on the other ring this loop
         * is a track ahead and blocking is the mechanism, not a fault
         * -- 15 s of it is a correctly working decode-ahead. Pause is
         * the same argument: the writer is stopped on purpose.
         */
        /* s_tail_pending as well as the index test: between a track's
         * decode ending and the next one's first block the indices are
         * equal and there is still a tail playing, which is the window
         * this warning fired in every time -- 16 s of correct
         * decode-ahead reported as a stall, once per boundary.
         *
         * OR'd with the state BEFORE the send, not just after it. The
         * send that blocks for eleven seconds is released BY the handoff
         * that clears these, so by the time the result is judged the
         * evidence has gone: 0403 tested them 44 ms too late and the
         * warning fired anyway. */
        /*
         * And a pause that arrived while the send was parked.
         *
         * `!s_playing` is tested both before and after, and a pause that
         * begins and ends inside one send is invisible to both: the
         * board logged `ring send blocked 30078 ms (ring 99%)` for a
         * thirty-second pause, reported at the moment of resuming. The
         * epoch counter is the only thing that can see an event rather
         * than a state -- the same lesson as 0403, one level further
         * out.
         */
        const bool paused_during = (s_pause_epoch != pause_epoch_before);
        const bool running_ahead = ahead_before || paused_during ||
                                   (s_ring_play != s_ring_fill) ||
                                   s_tail_pending || !s_playing;
        if (send_ms > LOOP_STALL_MS && !running_ahead) {
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
            /*
             * No drain. This is the gapless step.
             *
             * What is queued is the rest of this track and it will be
             * played out by the writer, which outlives this call. The
             * next play_file() fills the OTHER ring, so the two do not
             * collide, and the writer moves across when this one runs
             * dry -- at the sample, not at a convenient moment.
             *
             * Up to a ring of audio therefore outlives the loop that
             * produced it. Everything that describes the playing track
             * has to be published at the handoff rather than here; see
             * the visuals gate at the top of the loop.
             *
             * And this is where that is latched. blocks > 0 because a
             * track that decoded nothing never took a ring of its own,
             * so s_ring_fill still names the PREVIOUS track's ring and
             * latching against it would hold the screen for a tail that
             * is not this track's.
             */
            if (blocks > 0 && !xStreamBufferIsEmpty(s_ring[s_ring_fill])) {
                s_tail_ring = s_ring_fill;
                s_tail_pending = true;
                ESP_LOGI(TAG, "tail: %u KB of this track still to play; "
                              "holding the screen",
                         (unsigned)(xStreamBufferBytesAvailable(s_ring[s_ring_fill]) / 1024));
            }
        } else if (why == TRACK_MEDIA_GONE) {
            /*
             * The card left. The music did not.
             *
             * What is queued was decoded while the volume was still
             * there and needs nothing from it to be played -- so it is
             * played, and faded rather than cut. This is the one "not a
             * clean end" case where the queued audio is neither stale
             * nor unwanted: nobody chose to stop listening to it, and
             * the only thing that changed is that there will not be any
             * more after it.
             *
             * It does not play the whole ring out. Twenty seconds of
             * music continuing after the card is in your hand reads as
             * the player having ignored the removal, and the far end of
             * it is a hard stop anyway. The ramp starts now and silence
             * arrives FADE_OUT_MS later; the writer drops the rest when
             * it gets there.
             *
             * Against audio_out_rate(), not the file's rate: the ramp is
             * counted in frames and the frames are consumed by the
             * hardware clock. See fade_out_begin().
             */
            fade_out_begin(audio_out_rate());
            ESP_LOGI(TAG, "media gone; fading out %u KB of queued audio "
                          "over %" PRIu32 " ms",
                     (unsigned)(xStreamBufferBytesAvailable(s_ring[s_ring_play]) / 1024),
                     (uint32_t)FADE_OUT_MS);
            /*
             * The screen moves on at the press -- or here, at the
             * removal, which is the same kind of moment: an external
             * event that ends the track. The deferral exists to keep
             * the screen with audio that is still the track's, and
             * what is left of this one is a fade to silence and the
             * chooser coming up behind it.
             */
            s_tail_pending = false;
        } else {
            /*
             * Interrupted: what is queued is a track the user has
             * finished with, and playing 20 s of it after the tap
             * sounds like the tap was ignored.
             *
             * The writer may be parked on this ring or on the other
             * one. Resetting the one being filled is right either way:
             * if the writer is here it stops immediately, and if it is
             * still on the previous ring this one held nothing anybody
             * was going to hear.
             */
            /*
             * Through the writer, not with a reset. This site named the
             * hazard itself -- "the writer may be parked on this ring"
             * -- and that is precisely the case 0507 identified: a reset
             * rewrites head and tail under a task blocked inside
             * xStreamBufferReceive() on the same buffer. This is the
             * boundary that still flashed in the SEEK_KEEP_RING build,
             * because SEEK_KEEP_RING only skipped the seek path's reset
             * and a `prev` press comes through here.
             *
             * Asynchronous, where the reset was immediate. The writer
             * picks it up at the top of its next pass, which is at worst
             * one chunk of DMA away, and the deferral below is what the
             * screen was already waiting on.
             */
            s_flush_why = "interrupted";
            s_pcm_flush = true;
            /*
             * And the screen changes now, on the press, which is the
             * other of the two moments it is allowed to change on. The
             * queued audio this just dropped is what the deferral exists
             * to protect, and it has been dropped; there is nothing left
             * to be out of step with.
             */
            s_tail_pending = false;
        }

        /*
         * Nothing is deleted here any more, so none of the ordering the
         * delete needed applies: no wait for the writer to leave the
         * buffer, no -1 published ahead of a free, no handle cleared.
         * The ring outlives the track and the writer outlives both.
         *
         * The pause override is dropped again. It is set for the drain
         * above and means "ignore pause"; left set, the next pause
         * would not pause.
         */
        s_writer_stop = false;
        s_ring_pct = 0;
    }
    /*
     * What the next track needs to know about this one.
     *
     * Recorded before anything below can return, so every exit from
     * play_file() leaves it correct. `why` is the whole of the first
     * half: only a track that reached its own end is a boundary worth
     * softening, and everything else -- a skip, an unreadable file, a
     * pulled card -- is a boundary the listener caused or was told
     * about. See the arming block at the top.
     */
    /*
     * A SEEK THAT RAN OFF THE END IS NOT A TRACK THAT ENDED.
     *
     * The arming block at the top says so in words and this is what
     * makes it true. A drag to the last inch of the bar leaves under a
     * second of audio, the decoder reaches the end of it almost at once,
     * and `why` is TRACK_ENDED -- indistinguishable, from here, from a
     * track played through.
     *
     * It is not the same boundary. The listener just moved the playhead
     * themselves, so the next track arriving is a consequence of a press
     * and should sound like one; and with up to a ring of the old
     * position still queued, softening it means MIXING the tail of the
     * seeked-from audio with the incoming track. Where the incoming
     * track is the same file -- repeat-one, or a `next` back round to it
     * -- that is the same recording played over itself, which is how
     * 0700 was noticed.
     *
     * One second of decoded audio after the seek is the threshold. Below
     * it the seek is what ended the track; above it the track ran on and
     * ended on its own terms.
     */
    /*
     * Played to the end AND got there by decoding. Everything written
     * to the sidecar hangs off this rather than off `why` alone: `why`
     * answers what to play next, and a track that died mid-file still
     * wants the playlist to move on.
     */
    const bool complete = (why == TRACK_ENDED) && !decode_failed;
    if (decode_failed) {
        ESP_LOGW(TAG, "the decoder failed part way; nothing measured "
                      "here is being kept");
    }

    bool clean = (why == TRACK_ENDED);
    if (clean && seeked && cur_rate && frames_out - frames_at_seek < cur_rate) {
        ESP_LOGI(TAG, "the seek ran off the end; not a clean boundary");
        clean = false;
    }
    /*
     * THE LENGTH, WHEN NOTHING ELSE COULD SAY.
     *
     * A raw ADTS file that declares itself VBR has no duration from any
     * of the four seek mechanisms, so its bar stays a groove for ever.
     * An uninterrupted play measures it exactly, which is the same
     * trade the loudness and the envelope make.
     *
     * 0712 put this inside the loudness block, which was wrong in a way
     * that only showed on the SECOND play: `measuring` is false once
     * the sidecar already holds a loudness, so the file whose length
     * was still missing was exactly the file that no longer measured
     * anything. The board played 08 start to finish and logged nothing.
     *
     * A length is not a measurement of the audio, it is a count of what
     * came out, so it belongs here with the other facts about how the
     * track ended and depends on nothing but that: played to the end,
     * with no seek in it, on a rate that was known.
     */
    if (complete && !seeked && cur_rate && rg_holding(path) &&
        (!s_rg.format.present || !s_rg.format.sec)) {
        /*
         * Rounded, not truncated. The count is frames that reached the
         * ring, which is a hair under the file's real length once the
         * last partial block is accounted for -- and truncating that
         * turned a 60 s file into `length from playback: 59 s`. A
         * length is being reported to the nearest second on a bar
         * 720 px wide; half a second of rounding is under a pixel.
         */
        const uint32_t measured =
            (uint32_t)((frames_out + cur_rate / 2) / cur_rate);
        if (measured) {
            s_rg.format.present = true;
            s_rg.format.sec = measured;
            /* And the envelope's own span, which took its length from
             * the same missing number and would otherwise draw 375
             * columns across `0s`. */
            if (s_rg.waveform.present && !s_rg.waveform.sec) {
                s_rg.waveform.sec = measured;
            }
            s_rg_dirty = true;
            ESP_LOGI(TAG, "length from playback: %" PRIu32 " s", measured);
        }
    }

    /*
     * And the recorded table, on the same terms as the length: played
     * to the end, no seek in it. A partial table is worse than none --
     * a drag past where the recording stopped would land on the last
     * pair and read as the bar having ignored the press.
     */
    if (tbl_rec && complete && !seeked && tbl_n <= 1) {
        /* Loud, because the silent version of this cost fifteen plays.
         * A recording that reaches the end of a track with nothing in
         * it is a fault in the recording, not a property of the file. */
        ESP_LOGW(TAG, "adts: table recording produced %d entries; not stored",
                 tbl_n);
    }

    if (complete && !seeked && tbl_rec && tbl_n > 1 &&
        rg_holding(path)) {
        s_rg.index.present = true;
        s_rg.index.count = tbl_n;
        s_rg.index.spacing_sec = tbl_spacing;
        memcpy(s_rg.index.offset, tbl_off, (size_t)tbl_n * sizeof(uint32_t));
        memcpy(s_rg.index.sample, tbl_frm, (size_t)tbl_n * sizeof(uint32_t));
        s_rg_dirty = true;
        ESP_LOGI(TAG, "adts: table recorded, %d entries every %" PRIu32 " s",
                 tbl_n, tbl_spacing);
    }

    s_prev_ended_clean = clean;

    /*
     * `why` itself is deliberately NOT changed by the test above. It is
     * the caller's instruction about what to play next -- TRACK_ENDED
     * advances the playlist, TRACK_INTERRUPTED does not -- and a seek to
     * the last second of a track still wants the next one. The only
     * thing the distinction governs is the crossfade.
     */
    {
        snprintf(s_prev_path, sizeof(s_prev_path), "%s", path);
        const char *slash = strrchr(path, '/');
        if (slash) {
            const size_t len = (size_t)(slash - path);
            const size_t n = len < sizeof(s_prev_dir) - 1
                           ? len : sizeof(s_prev_dir) - 1;
            memcpy(s_prev_dir, path, n);
            s_prev_dir[n] = '\0';
        } else {
            s_prev_dir[0] = '\0';
        }
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

    /*
     * The seek table, harvested from whatever index the decoder ended up
     * holding.
     *
     * Outside the TRACK_ENDED test, deliberately, and outside the
     * loudness block it sits after: an index is a fact about where the
     * frames are, not a measurement of the audio, so a skipped track has
     * learned it just as completely as a track played through. That is
     * the same reasoning the tags and the art flag are written under.
     *
     * decoder_index_extract() returns false when the index came from
     * this record in the first place, so a file that already has a table
     * reads it, uses it, and writes nothing.
     *
     * The two ways one exists to be harvested are the two ways minimp3
     * builds one: the scan at open on a file with no table, and the
     * lazy build inside the first mp3dec_ex_seek() on a Xing-tagged
     * file, which never scans at open and pays for it on the first drag.
     * Both are whole-file walks the player has already spent; this is
     * what stops it spending them again next time.
     */
    if (rg_holding(path)) {
        uint32_t ixoff[REPLAYGAIN_INDEX_MAX];
        uint32_t ixfrm[REPLAYGAIN_INDEX_MAX];
        int ixn = 0;
        uint32_t ixsp = 0;
        if (decoder_index_extract(dec, ixoff, ixfrm, REPLAYGAIN_INDEX_MAX,
                                  REPLAYGAIN_INDEX_SPACING_SEC,
                                  &ixn, &ixsp)) {
            s_rg.index.present = true;
            s_rg.index.count = ixn;
            s_rg.index.spacing_sec = ixsp;
            memcpy(s_rg.index.offset, ixoff, (size_t)ixn * sizeof(uint32_t));
            memcpy(s_rg.index.sample, ixfrm, (size_t)ixn * sizeof(uint32_t));
            s_rg_dirty = true;
        }
    }

    /* Why, not just that. "finished" on a track the user skipped out of
     * reads as the skip having been ignored until the next line arrives. */
    /* A track that ran to the end without producing a single block did
     * not play. Reported as unreadable so the caller can count it, since
     * "finished, 0 blocks" and "finished, 261 blocks" mean opposite
     * things and only one of them is a reason to move on cheerfully. */
    /*
     * The other way a length gets learned: not from a walk but from the
     * decoder's own index, built during the open. A Xing-less MP3 is the
     * case -- decoder_duration_sec() answers only after minimp3_ex has
     * read the whole file, which is the 1.4 s "open took" in every log.
     *
     * Filed at the end rather than at the open so it goes out with the
     * loudness and the envelope in one write, and skipped entirely when
     * the container stated the length itself.
     */
    if (!len_from_header) rg_note_format(path, &track_info, len_sec, file_bytes);

    if (why == TRACK_ENDED && blocks == 0) why = TRACK_UNREADABLE;

    /*
     * The measurement is only written for a track that played from
     * start to finish with no seek in it -- which is what TRACK_ENDED
     * means by the time it survives the check above. Everything else
     * (skipped, interrupted, media pulled, opened but silent) is
     * discarded rather than written as a partial answer nothing could
     * later tell apart from a real one.
     *
     * A restart is not a special case: it re-enters play_file() for the
     * same path, which resets the accumulator, so the restarted play
     * gets its own clean attempt and writes if IT reaches the end.
     */
    if (measuring && complete) {
        float lufs = 0.0f, peak = 0.0f;
        uint32_t gated = 0;
        if (loudness_finish(&s_loud, &lufs, &peak, &gated)) {
            /* Zero gated blocks is not a quiet track, it is no track:
             * the gate never opened because nothing above the absolute
             * threshold arrived. Belt to the decode_failed braces --
             * whatever else goes wrong upstream, a loudness computed
             * from nothing does not get filed as one. */
            if (gated == 0) {
                ESP_LOGW(TAG, "loudness gated nothing; not stored");
            } else if (rg_holding(path)) {
                s_rg.loudness.present = true;
                s_rg.loudness.integrated_lufs = lufs;
                s_rg.loudness.sample_peak_dbfs = peak;
                s_rg.loudness.blocks = gated;
                s_rg_dirty = true;
            }
            ESP_LOGI(TAG, "loudness: %.2f LUFS, peak %.2f dBFS, %" PRIu32
                          " gated blocks",
                     (double)lufs, (double)peak, gated);

            /*
             * The envelope, from the same pass. This is where waveforms
             * come from now -- framewalk.c read global_gain out of the
             * frame headers, which is the encoder's bit budget and not
             * the audio; this is peak magnitude off the decoded PCM.
             *
             * Written only here, so a track has an envelope and a
             * loudness together or has neither, and both cost one
             * uninterrupted play rather than a whole-file read.
             */
            uint8_t env[LOUDNESS_ENV_COLUMNS];
            int cols = 0;
            loudness_envelope(&s_loud, env, &cols);
            /* Two columns, not one. A single column is not a waveform
             * of anything -- it is one block's peak stretched across
             * the whole width, which is what file 15 drew: `envelope:
             * 1 columns, levels 0..255` under a sixty second bar. */
            if (cols > 1 && gated && rg_holding(path)) {
                s_rg.waveform.present = true;
                /* len_sec, or the length just measured from this very
                 * play when the file never stated one -- otherwise the
                 * envelope written on the first complete play of an
                 * ADTS file spans `0s` and the record disagrees with
                 * itself. */
                s_rg.waveform.sec = len_sec ? len_sec : s_rg.format.sec;
                s_rg.waveform.columns =
                    (cols > REPLAYGAIN_COLUMNS) ? REPLAYGAIN_COLUMNS : cols;
                memcpy(s_rg.waveform.level, env,
                       (size_t)s_rg.waveform.columns);
                s_rg_dirty = true;
                ESP_LOGI(TAG, "envelope from playback: %d columns", cols);
            }
        }
    }

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

    /*
     * One write, here, for everything this track taught us -- and only
     * if it taught us something. A track played twice with nothing new
     * to record does not touch the card at all the second time.
     *
     * After the loudness merge above and outside the TRACK_ENDED test,
     * because a skipped track still learned its tags and whether it has
     * a cover even though its loudness was thrown away.
     */
    rg_release();

    /*
     * The gain belonged to this track. Left set, the bar would keep
     * claiming it during the gap before the next one starts.
     *
     * ...but only when there is no tail still being heard, which is the
     * same condition and the same reason as s_pos_sec below and as the
     * stats in track_change_begin(). This function returns when this
     * track's DECODE ends, and at a boundary that is a ring -- twenty
     * seconds -- before its last sample reaches the speaker. Clearing
     * here took the RG mark off the screen while the track it describes
     * was still playing.
     *
     * 0510 deferred the incoming track's gain to the handoff and this
     * is the other half: the outgoing one has to survive until the same
     * moment, or the indicator goes out at the decode boundary and
     * comes back at the audio one with a gap in between. Which is what
     * it did -- "the current one disappears, the new one isn't drawn
     * early" is exactly one of the two halves being fixed.
     *
     * The gate republishes both when the handoff happens, so nothing is
     * left stale: a track that ends with nothing following it clears
     * these through the not-playing path instead.
     */
    if (!tail_playing()) {
        s_rg_measuring = false;
        s_rg_active = false;
        s_rg_gain_db = 0.0f;
    }

    /* Not zeroed when the tail of this track is still queued: the
     * writer goes on playing it and pos_publish() goes on describing it
     * from the ring, which is the only thing that still knows where it
     * has got to. Zeroing here dropped the bar to the start for the last
     * twenty seconds of every song. */
    if (!tail_playing()) s_pos_sec = 0;
    return why;
}
#undef VISUALS_GATE

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

/* One attempt, whether or not it finds anything. A volume with no
 * settings file, or one naming a track that has been deleted, is not a
 * reason to keep asking every 100 ms for the rest of the session. */
static bool s_restored;

/*
 * Load the settings off whatever is mounted and put their track on
 * screen, stopped.
 *
 * Returns nothing because there is nothing useful to do about a
 * failure: every step is allowed to come up empty and the answer to all
 * of them is the chooser, which is already up.
 */
static void restore_last_track(void)
{
    for (int v = 0; v < STORAGE_COUNT; v++) {
        if (!storage_present((storage_id_t)v)) continue;

        const char *root = storage_mount_path((storage_id_t)v);
        if (!root || !settings_note_path(root)) continue;

        /* Whatever the file said, applied -- this is the same push
         * player_loop() does when a track starts, and at this point it
         * is the only one that has happened. */
        s_volume = settings_volume();
        audio_out_set_volume((uint8_t)s_volume);
        ESP_LOGI(TAG, "volume %d from settings", s_volume);

        s_restored = true;

        const char *last = settings_track();
        if (!last) return;

        /* The file may be gone, or on the other volume, or renamed.
         * Opening it is the only honest test and it is cheap. */
        FILE *probe = fopen(last, "rb");
        if (!probe) {
            ESP_LOGI(TAG, "last track is gone: %s", last);
            return;
        }
        fclose(probe);

        /* Its folder becomes the list, so next and prev work from the
         * first press without going through the chooser. */
        char dir[512];
        snprintf(dir, sizeof(dir), "%s", last);
        char *slash = strrchr(dir, '/');
        if (slash && slash != dir) {
            *slash = '\0';
            if (playlist_load_dir(dir) == ESP_OK) {
                const int i = playlist_index_of(last);
                if (i >= 0) playlist_set_current(i);
            }
        }

        snprintf(s_path, sizeof(s_path), "%s", last);
        load_track_visuals(s_path);
        ESP_LOGI(TAG, "ready to resume %s", s_path);

        /*
         * The chooser came up at boot with nothing mounted, so it has
         * no volume and no directory. Now there is a track, and the
         * chooser knows how to open at one -- same call the transport's
         * folder button makes, which lands on the right tab with the
         * right folder and the track marked.
         *
         * Reopened rather than closed: with nothing playing, a chooser
         * showing the folder you were in is more use than the bar for a
         * track that is stopped.
         */
        if (browser_is_open()) browser_open(s_path);
        return;
    }
}

/*
 * Take the track off the screen.
 *
 * Everything the bar draws about a song, cleared in one place: the
 * title row and the tags behind it, the filename it falls back to, the
 * cover, the envelope, the clock and the length, the seek affordance
 * and the ReplayGain mark. Not the volume, not the battery, not the
 * route -- those are the player's, not the track's, and they are still
 * true.
 *
 * There was no such function before this because there was no such
 * moment. A track leaving the screen was always a track arriving on it,
 * and load_track_visuals() overwrites the lot; the one exception, the
 * end of a playlist, leaves the last track up on purpose so you can see
 * where you got to. Media removal is the case where the track is gone
 * and nothing is coming, and leaving its cover and its waveform up over
 * a card that is in your hand is the screen asserting something false.
 *
 * The generation bump is not decoration. media_task may be mid-JPEG for
 * this track -- a single call into a decoder that cannot be cancelled --
 * and it checks the generation before it draws. Without the bump, a
 * cover decoded from a cache entry outlives the clear by a second and
 * paints itself back onto a blank screen.
 *
 * Called from player_loop() only. Everything in here touches the
 * framebuffer or the state the UI task reads while doing so.
 */
static void clear_play_screen(void)
{
    /* Ordered so the UI task cannot sample a half-cleared track: the
     * strings that name it go first, the numbers that describe it
     * after, and the flags that say those numbers are worth drawing
     * are already false by the time anything reads them. */
    s_stats_valid = false;
    s_can_seek    = false;
    s_pos_sec     = 0;
    s_len_sec     = 0;

    s_rg_active    = false;
    s_rg_measuring = false;
    s_rg_gain_db   = 0.0f;

    s_fmt_known = false;

    memset(&s_tags, 0, sizeof(s_tags));
    s_display_name = "";
    /* Nothing is playing, so there is no cover to wait for and no
     * reason to hold an empty title back from a screen that is being
     * cleared anyway. */
    memset(&s_tags_shown, 0, sizeof(s_tags_shown));
    s_name_shown = "";
    s_text_staged = false;
    s_path[0] = '\0';

    /* Anything still in flight for the track that just ended belongs to
     * a generation that no longer exists. See do_art()'s gen checks. */
    s_track_gen++;

    /* The two that actually paint. The envelope first, because it is
     * part of the bar and ui_clear_art() is the rest of the screen. */
    wave_clear();
    s_env_pending = ENV_PENDING_NONE;
    /* And the record of the work, which clear_play_screen() must drop
     * too: the file is on a volume that has gone, and a later card with
     * the same path on it is a different file. */
    s_walked_path[0] = '\0';
    ui_clear_art();

    ESP_LOGI(TAG, "the screen is empty; nothing is playing");
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

    /*
     * Chosen, not started.
     *
     * autostart() used to return straight into playback, so switching
     * the player on made noise. It now only picks what the transport
     * will play when it is pressed, which is also what
     * restore_last_track() does with a better answer if the settings
     * file has one -- and that runs a moment later, once a volume is
     * mounted, and overrides this.
     */
    bool have = false;
    if (autostart(s_path, sizeof(s_path))) {
        load_track_visuals(s_path);
        ESP_LOGI(TAG, "ready to play %s", s_path);
    } else {
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
            /*
             * The track this player was last put down on, restored but
             * not started.
             *
             * It has to happen here rather than in app_main(): the
             * settings file lives on the volume the music is on, and a
             * USB drive is still enumerating when app_main() runs. The
             * idle branch is where the player waits anyway, so it is
             * where the volume turning up is noticed.
             *
             * Deliberately not autoplay. A player that starts making
             * noise because it was switched on is a worse object than
             * one that shows you where you were and waits to be asked.
             * The transport is one press away and the track is already
             * on screen with its envelope and its gain.
             */
            if (!s_restored) restore_last_track();

            /*
             * The fade has reached silence. See s_fade_cleanup.
             *
             * Here rather than at the removal, which is the whole of
             * this patch: for the three seconds the ring is still being
             * heard, the screen goes on showing the track being heard.
             * It clears when the sound does, at the same moment and for
             * the same reason.
             *
             * The chooser comes up behind it, deferred from the
             * media-gone branch below for the same reason -- putting a
             * file listing over a track that is still audible answers a
             * question nobody has asked yet.
             *
             * The two extra terms close the window against a choice
             * made in the last moments of the ramp: fade_out_cancel()
             * drops the flag, but it can only do that before the writer
             * sets it, and a track chosen in between would otherwise
             * have its screen blanked a moment after it was drawn.
             * Neither of these is true when there is nothing playing,
             * which is every other time this runs.
             */
            if (s_fade_cleanup && !s_decoding && !s_pending_ready) {
                s_fade_cleanup = false;
                clear_play_screen();
                s_repaint_art = false;   /* just done, and s_path is gone */
                s_open_chooser = true;
            }

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

        /*
         * Settings live on the volume the music is on. Cheap and
         * idempotent -- it returns immediately unless the volume has
         * actually changed.
         *
         * The true return is the first adoption of the session, which
         * is where the file is read. app_main() samples settings_volume()
         * long before that can have happened, so without this the saved
         * volume was loaded into settings.c and never reached the codec:
         * the player kept the 50 it booted with, and the next drag saved
         * that over the file.
         */
        /*
         * settings_set_track() used to be here, next to its sibling, and
         * it wrote the resume point for a track twenty seconds before
         * any of it was audible: pull the power during the tail and the
         * player came back a track further on than the listener ever
         * got. It has moved to the visuals gate, which is where the rest
         * of "this is the track now playing" already lives.
         *
         * settings_note_path() stays. It is about which VOLUME the
         * settings file lives on, not which track is playing, and the
         * volume it adopts has to reach the codec before the first
         * sample rather than after it.
         */
        if (settings_note_path(s_path)) {
            s_volume = settings_volume();
            audio_out_set_volume((uint8_t)s_volume);
            ESP_LOGI(TAG, "volume %d from settings", s_volume);
        }

        ESP_LOGI(TAG, "playing %s", s_path);
        history_push(s_path);
        const track_end_t why = play_file(s_path);
        have = false;

        if (why == TRACK_INTERRUPTED) {
            /* Something else is already chosen, so the amplifier has a
             * reason to stay powered through the gap. See the block
             * below for why this is set out here rather than left set
             * by play_file(). */
            s_track_changing = true;
            continue;                             /* s_pending has the next */
        }

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
            /*
             * The chooser waits for the fade, and so does the clear.
             *
             * Both are deferred to the idle branch above, which fires
             * when the writer says the ramp has reached silence. Until
             * then the track is still coming out of the speaker and the
             * screen showing it is the truth.
             *
             * If no ramp is running -- the ring was empty, or the fade
             * was refused -- there is nothing to wait for and the flag
             * is set here instead, so the deferral can never be the
             * reason a dead track stays on screen for ever.
             */
            if (!fade_out_active()) s_fade_cleanup = true;
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
            /*
             * There is another track, so this gap is a gap and not a
             * stop. Without this the amplifier idles across every
             * boundary on an album:
             *
             *   I (327491) finished, 17464 blocks
             *   I (329121) tab5_audio: amplifier off (idle)
             *   I (329343) tab5_audio: amplifier on
             *
             * -- powered down and back up 222 ms apart, which is an
             * audible click between two consecutive songs. audio_out.c
             * holds the shutdown for 1500 ms precisely to absorb a
             * track gap, and the gap is now about 1.8 s: drain, then
             * the sidecar write, then the next open. Just over.
             *
             * Set here rather than left set by play_file(), which
             * clears s_track_changing on its way out on purpose -- a
             * track that fails to open must not leave the amplifier
             * powered forever waiting for a sound that is not coming.
             * The loop is the only thing that knows there is a next
             * track, so the loop is what says so. If that next track
             * then fails to open, play_file() clears it again and the
             * amp idles as it should.
             */
            s_track_changing = true;
            snprintf(s_path, sizeof(s_path), "%s", next);
            have = true;
            continue;
        }

        /*
         * End of the folder, or single-track mode.
         *
         * "Nothing is playing and nothing is queued" is what this used
         * to say, and with one ring it was true: the boundary was a
         * drain, so a decode loop that had returned meant a track that
         * had been heard. It is false now. The last track of a folder
         * ends its decode with up to twenty seconds still in the ring,
         * and opening the chooser here put the file list over the top of
         * a song that was still playing -- the album ending on the menu
         * screen twenty seconds early, every time.
         *
         * So wait for it. A press during the wait wins: s_pending_ready
         * means something else has been chosen, and the loop above is
         * where that belongs.
         */
        while (s_tail_pending && !s_pending_ready) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (s_pending_ready) continue;

        /*
         * The last track has now been heard, so the indicator that
         * described it can go.
         *
         * play_file() no longer clears these when a tail is still
         * playing -- see the note there -- which is right for a boundary
         * and leaves exactly one case uncovered: the end of a folder,
         * where nothing follows to republish them at a handoff. This is
         * that case, and it is after the wait above, so "heard" is
         * already true when it runs.
         */
        s_rg_measuring = false;
        s_rg_active = false;
        s_rg_gain_db = 0.0f;

        /* And the chooser's marker, for the same reason and at the same
         * moment: nothing is being heard now, so no row is the playing
         * one. Nothing follows to republish it. */
        browser_set_playing(NULL);

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
    /*
     * The line to start copying from.
     *
     * Everything above this is the ROM, the bootloader and IDF's own
     * startup, which is the same on every boot and is not what anybody
     * is looking at when they paste a log into a bug report. Everything
     * below it is this program. The version is on the banner because a
     * log without one cannot be matched to a tree -- a -dirty suffix
     * here has already been the difference between a decoded backtrace
     * that meant something and one that pointed at a comment.
     *
     * ESP_LOGW rather than I so it survives a build that has turned the
     * info level down, and printed before anything else in app_main()
     * so that a failure in the very first init is still below the
     * banner.
     */
    {
        const esp_app_desc_t *d = esp_app_get_description();
        ESP_LOGW(TAG, "=== Defeatist Music Player === %s, IDF %s, built %s %s",
                 d ? d->version : "?", d ? d->idf_ver : "?",
                 d ? d->date : "?", d ? d->time : "?");
    }

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
    for (int i = 0; i < PCM_RINGS; i++) {
        s_pcm_store[i] = heap_caps_malloc(PCM_RING_BYTES + 1,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_pcm_store[i]) {
            ESP_LOGE(TAG, "could not reserve %u KB for PCM ring %d",
                     (unsigned)(PCM_RING_BYTES / 1024), i);
        }
    }

    /*
     * The ring and the writer, once, for the life of the program.
     *
     * Both were made and destroyed inside play_file() on every track,
     * which made a track boundary a teardown: drain, stop the writer,
     * wait for it to leave the buffer, delete. Nothing can be decoded
     * ahead across a boundary shaped like that, because there is a
     * window in it with no ring at all.
     *
     * Static create over storage that outlives everything, so this
     * allocates nothing and there is no delete to pair it with. A NULL
     * here is a player that cannot play, which play_file() reports per
     * track rather than refusing to boot -- the chooser still works and
     * still says what is on the card.
     */
    for (int i = 0; i < PCM_RINGS; i++) {
        if (!s_pcm_store[i]) continue;
        s_ring[i] = xStreamBufferCreateStatic(PCM_RING_BYTES, PCM_CHUNK_BYTES,
                                              s_pcm_store[i], &s_pcm_struct[i]);
    }
    /* Both or neither. One ring would play, but every boundary would
     * then behave differently from every other boundary, which is worse
     * than a boot that says plainly it cannot play. */
    s_ring_fill = s_ring_play = 0;
    s_pcm = (s_ring[0] && s_ring[1]) ? s_ring[0] : NULL;
    if (s_pcm) {
        xTaskCreate(i2s_writer_task, "i2s_wr", 4096, NULL, 6, NULL);
    } else {
        ESP_LOGE(TAG, "no PCM ring; this boot cannot play audio");
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

    ESP_ERROR_CHECK(storage_init());

    /*
     * Starts the writer task; loads nothing, because which volume the
     * settings live on is not known until a track plays. What it leaves
     * here is the built-in default, pushed at audio_out.c so the codec
     * and the player agree on something. The real value arrives at the
     * first settings_note_path() in player_loop().
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
    /*
     * 16 KB, not 8. The frame that overflowed the old stack has been
     * put on the heap, but play_file() still carries a replaygain_t and
     * a framewalk_t together and is only a little smaller than the one
     * that panicked. A task whose call graph passes 3 KB records around
     * wants more headroom than 8 KB leaves it.
     */
    mediacache_init();
    xTaskCreate(media_task, "media", 16384, NULL, 1, NULL);

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
