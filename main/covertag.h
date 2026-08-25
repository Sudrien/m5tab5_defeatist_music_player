/*
 * covertag.h -- cover art and text tags for the containers that are not
 * an MP3 with an ID3v2 tag on the front.
 *
 * albumart.c already had the ID3v2 reader, and it stays there: it is
 * bound up with the APIC frame layout and the v2.3/v2.4 size trap, and
 * moving it would have been churn for its own sake. What this file adds
 * is one parser per remaining container, plus the dispatcher that picks
 * between them by magic bytes -- so player.c asks for "the cover" rather
 * than asking for "the APIC frame" and getting nothing on a FLAC.
 *
 * The dispatcher is the point. Every one of these formats stores the
 * same two things, a picture and three strings, and the caller does not
 * care which. Before this, `do_art()` logged "no cover art in tag" for
 * every FLAC on the card, which was true about ID3 and false about the
 * file.
 *
 * Covered here:
 *
 *   FLAC   PICTURE (block type 6) and VORBIS_COMMENT (type 4)
 *   MP4    moov.udta.meta.ilst covr / (c)nam / (c)ART / (c)alb
 *   Ogg    VorbisComment in the Vorbis or Opus comment header, including
 *          a base64 METADATA_BLOCK_PICTURE
 *   WAV    an 'id3 ' chunk, handed back to albumart.c
 *
 * All four also accept an ID3v2 tag where one can legally appear, which
 * is why albumart.c grew the _at() variants: a FLAC with an ID3 tag
 * bolted on the front by a tagger is common enough to be worth not
 * failing on.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"

#include "albumart.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Biggest picture this will hand back.
 *
 * Not a decode limit -- albumart_show() copes with more -- but an
 * allocation limit, because the size comes out of the file and a corrupt
 * length field is otherwise a malloc() of whatever the corruption says.
 * 4 MB is past any real cover and well inside PSRAM.
 */
#define COVERTAG_MAX_IMAGE  (4u * 1024 * 1024)

/*
 * Cover art from any supported container, chosen by magic bytes.
 *
 * Returns ESP_ERR_NOT_FOUND when the file has no picture, and
 * ESP_ERR_NOT_SUPPORTED when the container is one there is no parser
 * for. On success the caller owns *out and must free() it.
 *
 * Leaves the file position undefined; callers sharing a handle with a
 * decoder must open their own, as they already had to.
 */
esp_err_t covertag_extract_art(FILE *f, uint8_t **out, size_t *out_len);

/*
 * Title, artist and album from any supported container. Same contract as
 * id3_read_tags(): fields are empty strings when absent, and the caller
 * falls back to the filename.
 */
esp_err_t covertag_read_tags(FILE *f, id3_tags_t *out);

#ifdef __cplusplus
}
#endif
