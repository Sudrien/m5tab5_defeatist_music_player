/*
 * recorder -- the record button: the two built-in microphones to a FLAC
 * file on the card (5106).
 *
 * 48 kHz, 24-bit, stereo (MIC1 left, MIC2 right), exactly as the ES7210
 * delivers it: no beamforming, no mixing to mono, no gain beyond the
 * ADC's own PGA. Files go to <volume>/Recordings/, the SD card if one is
 * mounted and the USB drive otherwise, named for the clock's UTC time
 * ("2026-09-26 18.04.33.flac") -- which before NTP is only the card's
 * floor (cardtime.h), so a best guess, as the README says.
 *
 * Two tasks for the length of a recording, created at start and gone at
 * the end: `rec_in` reads the I2S DMA into a 2 s PSRAM ring and never
 * touches the card; `rec_enc` drains the ring through flacenc.c into the
 * file. A card that stalls for a second costs ring, not samples; a ring
 * that fills is counted and logged as dropped audio, never silently.
 *
 * Playback is held paused while this runs (the player's side of it):
 * the microphones take the I2S port away from the DAC. See
 * audio_out_capture_begin().
 *
 * All of it from ui_task except the tasks themselves.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     active;        /* start() succeeded and the file is not yet closed */
    bool     stopping;      /* stop asked, the file is being finished */
    uint32_t seconds;       /* audio encoded so far */
    uint64_t bytes;         /* written to the file so far */
    uint32_t dropped_ms;    /* audio lost to a full ring, total */
    char     name[40];      /* the file's name, without the folder */
} recorder_status_t;

/*
 * Opens the file, writes the FLAC header, and starts both tasks. False,
 * with a reason for the notice card in why (may be NULL), when there is
 * no volume, the folder or file cannot be made, or the microphones
 * cannot be brought up.
 */
bool recorder_start(char *why, size_t why_len);

/* Asks for the end. The file is finished and closed on rec_enc; active
 * stays true until it is. */
void recorder_stop(void);

bool recorder_active(void);
void recorder_status(recorder_status_t *out);

/*
 * A card for the panel, once: why a recording ended by itself (the card
 * filled or went away, the 4 GB cap), or where a finished one went.
 * False when there is nothing new.
 */
bool recorder_take_notice(char *head, size_t head_len, char *body, size_t body_len);

#ifdef __cplusplus
}
#endif
