/*
 * uac.h -- USB Audio Class output.
 *
 * A speaker on the USB-A port. Output only: the RX (microphone) half of
 * the class is deliberately not opened, because this program has nothing
 * to do with a microphone and an open RX interface costs isochronous
 * bandwidth and a ring buffer for a stream nobody reads.
 *
 * The unit here is the interface, not the device. A headset enumerates
 * as two logical UAC devices -- one Audio Streaming interface each --
 * and the driver's connect callback fires once per interface. So
 * "a UAC device is attached" means "a TX interface has been opened",
 * which is the only sense in which it matters here.
 *
 * Nothing in this file resamples. If the device cannot take the format
 * the decoder is producing, uac_stream_start() says so and the caller
 * routes elsewhere -- see audio_out.c. Playing a 44.1 kHz file into a
 * 48 kHz-only device by handing it the bytes anyway is a semitone flat
 * and sounds like a broken player rather than like an unsupported rate.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Register the UAC class driver with usbhost.c.
 *
 * Must run before usbhost_start(), like every other class driver: one
 * registered afterwards is refused rather than installed late, because
 * it would never be offered a device that was already attached.
 */
esp_err_t uac_init(void);

/* A TX (speaker) interface is open. Not the same as streaming: a device
 * can be attached and idle, which is what it is between tracks. */
bool uac_present(void);

/* Bumped on every attach and every detach. Something that wants to
 * notice a headset appearing mid-track watches this rather than polling
 * uac_present(), so it can tell "still absent" from "gone and come
 * back". */
uint32_t uac_generation(void);

/*
 * Start (or restart) the stream in this format.
 *
 * 16-bit only, and the channel count is whatever the caller is handing
 * over -- no downmix here, for the same reason there is no resampler.
 *
 * Returns:
 *   ESP_OK                  streaming; uac_write() may be called
 *   ESP_ERR_NOT_FOUND       no device attached
 *   ESP_ERR_NOT_SUPPORTED   attached, but offers no alternate setting
 *                           matching this rate and channel count
 *   anything else           the driver's error from open or start
 *
 * Idempotent for a format already streaming, so the caller can ask on
 * every track without checking first.
 */
esp_err_t uac_stream_start(uint32_t rate, uint8_t channels);

/* Stop streaming; the interface stays open. Safe when not streaming. */
void uac_stream_stop(void);

bool uac_streaming(void);

/*
 * Hand over interleaved 16-bit PCM in the format uac_stream_start()
 * accepted.
 *
 * Blocks up to timeout_ms for room in the driver's ring. A timeout is
 * ESP_ERR_TIMEOUT and the bytes are dropped rather than partially
 * written -- the driver's write is all-or-nothing and there is no
 * partial-write count to resume from.
 *
 * ESP_ERR_INVALID_STATE means the device went away between the check
 * and the write, which is the normal way a headset is unplugged and is
 * the caller's cue to re-route rather than an error to report.
 */
esp_err_t uac_write(const void *data, size_t len, uint32_t timeout_ms);

/*
 * 0-100, matching the player's own scale.
 *
 * ESP_ERR_NOT_SUPPORTED when the device has no volume control the driver
 * can reach, which is common on the cheap C-Media parts. The caller is
 * expected to apply gain in software in that case rather than leaving
 * the slider dead -- see audio_out.c.
 */
esp_err_t uac_set_volume(uint8_t percent);

/* The device's product string, or "" -- for the log and the format card.
 * Never NULL. */
const char *uac_product(void);

#ifdef __cplusplus
}
#endif
