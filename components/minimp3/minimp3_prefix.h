/*
 * Rename every symbol minimp3 exports.
 *
 * esp_audio_codec's MP3 decoder is also minimp3, and it ships as a
 * precompiled archive -- lib/esp32p4/libesp_audio_codec.a, member
 * esp_mp3_dec.c.obj. That archive was built before we got here, so
 * mp3dec_init and mp3dec_decode_frame are already in it under those
 * exact names and no configuration on our side can take them out.
 * CONFIG_AUDIO_DECODER_MP3_SUPPORT=n gates their *registration* source,
 * not the archive member, which is why turning it off did not help.
 *
 * So we move instead of asking them to. Including this before
 * minimp3_ex.h textually renames our copy to tab5_mp3dec_*, and the two
 * decoders stop being the same symbol. Ours is the one decoder.c calls;
 * theirs stays in the binary, unreferenced and unreachable.
 *
 * This list is every non-static function in minimp3.h and minimp3_ex.h.
 * If minimp3 is ever updated and the link breaks again, diff the
 * declarations at the top of both headers against this list.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

/* minimp3.h */
#define mp3dec_init             tab5_mp3dec_init
#define mp3dec_decode_frame     tab5_mp3dec_decode_frame
#define mp3dec_f32_to_s16       tab5_mp3dec_f32_to_s16

/* minimp3_ex.h -- buffer and callback API */
#define mp3dec_detect_buf       tab5_mp3dec_detect_buf
#define mp3dec_detect_cb        tab5_mp3dec_detect_cb
#define mp3dec_load_buf         tab5_mp3dec_load_buf
#define mp3dec_load_cb          tab5_mp3dec_load_cb
#define mp3dec_iterate_buf      tab5_mp3dec_iterate_buf
#define mp3dec_iterate_cb       tab5_mp3dec_iterate_cb
#define mp3dec_ex_open_buf      tab5_mp3dec_ex_open_buf
#define mp3dec_ex_open_cb       tab5_mp3dec_ex_open_cb
#define mp3dec_ex_close         tab5_mp3dec_ex_close
#define mp3dec_ex_seek          tab5_mp3dec_ex_seek
#define mp3dec_ex_read_frame    tab5_mp3dec_ex_read_frame
#define mp3dec_ex_read          tab5_mp3dec_ex_read

/* minimp3_ex.h -- stdio API. Compiled out by MINIMP3_NO_STDIO, listed
 * anyway so that removing that define does not silently reintroduce a
 * collision. */
#define mp3dec_detect           tab5_mp3dec_detect
#define mp3dec_load             tab5_mp3dec_load
#define mp3dec_iterate          tab5_mp3dec_iterate
#define mp3dec_ex_open          tab5_mp3dec_ex_open
