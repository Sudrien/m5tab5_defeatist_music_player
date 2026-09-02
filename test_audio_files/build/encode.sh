#!/bin/sh
#
# encode.sh -- rebuild the seek test folder from landmark.wav.
#
# gen.py writes landmark.wav and nothing else. Every file in the folder
# above is that one minute of audio put through ffmpeg, and until now
# the commands existed only as the sentence "generated with ffmpeg
# 6.1.1" at the bottom of the README. A test corpus nobody can rebuild
# is a corpus that quietly stops matching what it claims to test.
#
# Usage:  ./encode.sh [outdir]      (default: out)
#
# Verified against ffmpeg 6.1.1. Files 01-05 and 08-14 come out
# byte-identical to the ones in the folder. 06 and 07 are the same size
# to the byte and differ inside it, because an Ogg stream carries a
# serial number that is random per run. 12 needs a cover image and is
# the one file that does not reproduce -- see the note where it is
# built.
#
# SPDX-License-Identifier: MIT
set -e

OUT="${1:-out}"
mkdir -p "$OUT"

[ -f landmark.wav ] || python3 gen.py

F="ffmpeg -loglevel error -y -i landmark.wav"

# 01/02 -- PCM at two sample widths. 02 is there because its block align
# is not a power of two, which is the arithmetic cbrseek.c has to get
# right rather than assume.
$F -c:a pcm_s16le "$OUT/01 wav-pcm16.wav"
$F -c:a pcm_s24le "$OUT/02 wav-pcm24.wav"

# 03 -- CBR with the Xing header suppressed, so minimp3 has to scan at
# open. 04 -- VBR with one, so it does not.
$F -c:a libmp3lame -b:a 192k -write_xing 0 "$OUT/03 mp3-cbr-noxing.mp3"
$F -c:a libmp3lame -q:a 4 "$OUT/04 mp3-vbr-xing.mp3"

$F -c:a flac "$OUT/05 flac.flac"

# 06/07 -- the two Ogg codecs. Vorbis for the multi-page codebooks in
# the preamble, Opus for the 48 kHz granule and the pre-skip. Opus is
# left at libopus's default rate; naming one changes the file.
$F -c:a libvorbis -q:a 4 "$OUT/06 ogg-vorbis.ogg"
$F -c:a libopus "$OUT/07 opus.opus"

# 08 -- raw ADTS. ffmpeg's AAC encoder writes buffer_fullness = 0x7FF in
# every header, which is the stream declaring itself variable, and that
# declaration is what this file is here to exercise.
$F -c:a aac -b:a 128k -f adts "$OUT/08 aac-adts.aac"

# 09 -- default mux, which puts mdat before moov. mp4seek.c reads the
# tables itself and does not care; the M4A parser it replaced did.
# 10 -- ALAC, and therefore +faststart, because that parser is still the
# one playing it and it refuses moov-last.
$F -c:a aac -b:a 128k "$OUT/09 m4a-aac.m4a"
$F -c:a alac -movflags +faststart "$OUT/10 m4a-alac.m4a"

$F -c:a aac -b:a 128k -f mpegts "$OUT/11 ts-aac.ts"

# 12 -- the same encode as 03 with a 600x600 cover ahead of the audio,
# so the sync scan has something to skip. THE ONE FILE THAT DOES NOT
# REPRODUCE BYTE-FOR-BYTE: the shipped copy carries a 174656-byte tag
# from an image that was not kept, and any cover of roughly that size
# tests the same thing. testsrc2 is used so the command needs nothing
# from outside the folder.
ffmpeg -loglevel error -y -f lavfi \
    -i "testsrc2=size=600x600:duration=1:rate=1" \
    -frames:v 1 -c:v png "$OUT/cover.png"
ffmpeg -loglevel error -y -i landmark.wav -i "$OUT/cover.png" \
    -map 0:a -map 1:v -c:a libmp3lame -b:a 192k -write_xing 0 \
    -c:v copy -id3v2_version 3 \
    -metadata:s:v title="Album cover" \
    -metadata:s:v comment="Cover (front)" \
    "$OUT/12 mp3-bigart-noxing.mp3"
rm -f "$OUT/cover.png"

# 13 -- shorter than the seek bar is wide. 14 -- mono at half rate.
$F -t 3 -c:a flac "$OUT/13 flac-short-3s.flac"
$F -ac 1 -ar 22050 -c:a pcm_s16le "$OUT/14 wav-mono-22k.wav"

echo "built into $OUT"
