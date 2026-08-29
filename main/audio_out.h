/*
 * audio_out.h -- where the PCM goes.
 *
 * Everything downstream of the ring: the I2S channel, the ES8388, the
 * speaker amp's enable, and the headphone-detect line that arbitrates
 * between the last two.
 *
 * It moved out of player.c because it stopped being one path. There is a
 * second output on the USB-A port now, and a player.c that knew about
 * both would be deciding which one is playing in the middle of a decode
 * loop -- which is a routing question and has nothing to do with
 * decoding.
 *
 * The contract with the caller is deliberately narrow:
 *
 *   - PCM is interleaved 16-bit, and it is stereo. The decode loop
 *     already duplicates mono into both slots before it reaches the
 *     ring, so this side never sees a mono block. The channel count is
 *     still a parameter rather than a constant because the sink has to
 *     state the format to the device, and a hardcoded 2 buried three
 *     files away from the thing that makes it true is how that stops
 *     being true.
 *   - The rate is stated before any audio in it is written, and changing
 *     it mid-stream is allowed but is the caller's business to drain
 *     around.
 *   - Nothing here resamples or mixes. One format at a time, whatever
 *     the file is.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bring up the I2S channel and the codec, and start the headphone-detect
 * task.
 *
 * exp1 is the PI4IOE5V6416 at 0x43: SPK_EN is P1 of it and the detect
 * line is P7. Must be called after io_expanders_init(), which is what
 * takes that expander out of reset.
 *
 * rate is only the rate MCLK starts at. The ES8388 will not answer
 * sensibly on I2C until MCLK is running, so the channel is started
 * before the codec is configured and something has to be picked; the
 * first track overwrites it.
 */
esp_err_t audio_out_init(i2c_master_bus_handle_t bus,
                         i2c_master_dev_handle_t exp1,
                         uint32_t rate);

/*
 * State the format of everything written from here until the next call.
 *
 * The caller must have drained whatever it had queued: reconfiguring the
 * I2S clock needs the channel disabled, and doing that with audio in the
 * DMA descriptors is an audible click.
 *
 * Idempotent for a format already set.
 */
esp_err_t audio_out_set_format(uint32_t rate, uint8_t channels);

/*
 * Whether this output stage can be clocked at that sample rate.
 *
 * Asked before a track is committed to, so an unplayable file is
 * skipped with a line rather than aborting the player. See
 * I2S_MAX_RATE_HZ in audio_out.c for why the ceiling is where it is.
 */
bool audio_out_rate_supported(uint32_t rate);

/* What the output stage is clocked at now. 0 before the first track. */
uint32_t audio_out_rate(void);

/* Interleaved 16-bit PCM in the format last stated. Blocks until the
 * hardware has taken all of it. */
esp_err_t audio_out_write(const void *data, size_t len);

/* 0-100. */
esp_err_t audio_out_set_volume(uint8_t percent);

/*
 * Silence without forgetting the level.
 *
 * Applied at whichever output holds the route, and re-applied when the
 * route changes -- muting and then unplugging a USB headset must not
 * come back on the speaker.
 */
void audio_out_set_mute(bool muted);
bool audio_out_muted(void);

/*
 * Nothing is being written -- paused, or no track open.
 *
 * Shuts down the analog output stage: the NS4150B's enable and the
 * codec's DAC. An amplifier driving silence is still an amplifier, and
 * the board has an audible whine on the speaker while charging that it
 * has no business producing with nothing playing.
 *
 * The USB-A port is NOT affected. VBUS stays up and a UAC device stays
 * enumerated and streaming a paused stream; cutting bus power here would
 * unplug a headset every time somebody hit pause, and re-enumerating on
 * play is seconds of nothing. This is the amplifier, not the port.
 *
 * Call it freely -- it is a compare when the state has not changed.
 * Going idle is deferred (see IDLE_HOLD_MS); coming back is immediate,
 * because the first moments of a track must not be clipped.
 */
void audio_out_set_idle(bool idle);

/* "speaker", "headphones" -- for the log. Never NULL. */
const char *audio_out_route_name(void);

#ifdef __cplusplus
}
#endif
