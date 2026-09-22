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



# ---------------------------------------------------------------- 0801
# Files for the mechanisms the first fourteen never reached.

# 15 -- CBR ADTS, and the one file here that ffmpeg cannot make.
#
# cbrseek.c's ADTS branch needs a stream that declares a real
# buffer_fullness and holds a steady rate. ffmpeg's AAC encoder writes
# 0x7FF in every header -- that is file 08, and 08's refusal is what 08
# is for -- so this needs an encoder ffmpeg does not ship.
#
# 0801 faked it by padding every frame to a constant length. That was
# wrong on hardware: the padding is zero bytes after the raw data
# block's terminator, and a zero byte is not nothing to an AAC decoder,
# it is ID_SCE. ffmpeg stops at the terminator and never looks;
# Espressif's decoder reads on to the declared frame length and tries
# to decode a channel out of the zeros. "Failed to decode aac frame,
# error:30", one block, and a corpse of a sidecar. Verified transparent
# against the wrong decoder, which is the whole lesson.
#
# So: fdk-aac, built from source, about two minutes.
#
#   git clone https://github.com/mstorsjo/fdk-aac
#   cd fdk-aac && cmake -S . -B build -DBUILD_PROGRAMS=ON \
#       -DBUILD_SHARED_LIBS=OFF && cmake --build build
#
# and put build/aac-enc on PATH. Without it this one file is skipped
# and the other twenty still build.
if command -v aac-enc >/dev/null 2>&1; then
    aac-enc -r 128000 -t 2 -v 0 landmark.wav "$OUT/15 aac-adts-cbr.aac"
else
    echo "skipping 15: aac-enc not on PATH (see the note above)" >&2
fi

# 16 -- fragmented MP4. mp4seek.c looks for moov/trak/mdia/minf/stbl and
# nothing else; a file whose sample tables live in moof boxes has no
# stbl to find, so the probe should fail and the file should fall back.
$F -c:a aac -b:a 128k -movflags frag_keyframe+empty_moov \
    "$OUT/16 m4a-aac-fragmented.m4a"

# 17 -- a transport stream spliced from two sources, so the PTS restarts
# part way and the last timestamp is not after the first. tsseek.c
# refuses a non-monotonic key at the probe; ffprobe reads this file as
# 91873 seconds long, which is what searching it anyway would look like.
ffmpeg -loglevel error -y -i landmark.wav -t 30 -c:a aac -b:a 128k \
    -output_ts_offset 3600 -f mpegts "$OUT/tmp-a.ts"
ffmpeg -loglevel error -y -ss 30 -i landmark.wav -c:a aac -b:a 128k \
    -output_ts_offset 0 -f mpegts "$OUT/tmp-b.ts"
cat "$OUT/tmp-a.ts" "$OUT/tmp-b.ts" > "$OUT/17 ts-aac-spliced.ts"
rm -f "$OUT/tmp-a.ts" "$OUT/tmp-b.ts"

# 18 -- two Ogg streams end to end, which is a legal chained file and
# what a concatenated podcast looks like. The tail window holds no page
# of the stream being played, which is what 0802 fixed.
#
# THE HALVES ARE 20 AND 40 SECONDS AND MUST NOT BE EQUAL. They were 30
# and 30, and duration.c read the granule off the last page in the file
# without checking whose it was -- which gave 30, the right answer for
# the wrong reason, and the file certified a bug as passing. Unequal
# halves make the two answers 20 and 40, and only one of them can be
# printed.
ffmpeg -loglevel error -y -i landmark.wav -t 20 -c:a libvorbis -q:a 4 \
    "$OUT/tmp-a.ogg"
ffmpeg -loglevel error -y -ss 20 -i landmark.wav -c:a libvorbis -q:a 4 \
    "$OUT/tmp-b.ogg"
cat "$OUT/tmp-a.ogg" "$OUT/tmp-b.ogg" > "$OUT/18 ogg-vorbis-chained.ogg"
rm -f "$OUT/tmp-a.ogg" "$OUT/tmp-b.ogg"

# ---------------------------------------------------------------- 0803
# Sample widths other than 16.

# 19/20 -- 32-bit integer and 32-bit float, the same ten seconds of the
# same audio. esp_audio_simple_dec_info_t reports a bit count and no way
# to tell these two apart, which is the whole argument for 32 staying
# refused while 24 is folded: there is no safe guess. Ten seconds rather
# than sixty because they are 350 KB a second and neither is ever meant
# to reach the speaker.
ffmpeg -loglevel error -y -i landmark.wav -t 10 -c:a pcm_s32le \
    "$OUT/19 wav-pcm32.wav"
ffmpeg -loglevel error -y -i landmark.wav -t 10 -c:a pcm_f32le \
    "$OUT/20 wav-float32.wav"

# 21 -- 24-bit FLAC, which is what the fold is actually for. 02 proves
# the arithmetic against a 16-bit twin; this proves the path works when
# the 24 bits arrive from a real decoder rather than from a PCM chunk.
# It is 951 KB because tones compress, not because it is short.
$F -c:a flac -sample_fmt s32 "$OUT/21 flac-24bit.flac"

# ---------------------------------------------------------------- 1002
# Cue sheets. The player does not read them yet; these are the files
# the feature will be written against, so the corpus exists before the
# code rather than being made to fit it afterwards.
#
# Every sheet puts its track starts somewhere the landmark makes
# audible: on a ten-second mark the track opens with that mark's beeps,
# and anywhere else the tick and the pitch say where it began. The
# README has what each one should do.
#
# The .cue files are written byte by byte with printf, not by an
# editor, because three of the things they test are bytes: a UTF-8 BOM,
# CRLF line ends, and a Windows-1252 accent.

# crlf FILE -- rewrite FILE with CRLF line ends, as EAC writes them.
crlf() { sed 's/$/\r/' "$1" > "$1.tmp" && mv "$1.tmp" "$1"; }

# 22 -- the ordinary case: one FLAC image of a disc, six tracks on the
# six ten-second marks, so track N opens with N beeps. UTF-8 with a BOM
# and CRLF, which is what EAC and foobar2000 write today, and one title
# outside ASCII.
$F -c:a flac "$OUT/22 cue-flac-image.flac"
{
    printf '\357\273\277'
    printf 'REM GENRE "Test"\n'
    printf 'REM DATE 2026\n'
    printf 'REM COMMENT "defeatist test corpus"\n'
    printf 'PERFORMER "Landmark Ensemble"\n'
    printf 'TITLE "Six Marks"\n'
    printf 'FILE "22 cue-flac-image.flac" WAVE\n'
    n=1
    for t in "One Beep" "Two Beeps" "Three Beeps" "Four Beeps" \
             "F\303\274nf Signalt\303\266ne" "Six Beeps"; do
        printf '  TRACK %02d AUDIO\n' $n
        # %b, not %s: the fifth title's accents are octal escapes, and
        # %s writes them out as backslashes. 1003.
        printf '    TITLE "%b"\n' "$t"
        printf '    PERFORMER "Landmark Ensemble"\n'
        printf '    INDEX 01 00:%02d:00\n' $(( (n - 1) * 10 ))
        n=$((n + 1))
    done
} > "$OUT/22 cue-flac-image.cue"
crlf "$OUT/22 cue-flac-image.cue"

# 23 -- the index lines that are not "track starts here". Windows-1252,
# no BOM, LF: an older EAC sheet, and the accent in the performer is a
# byte that is not valid UTF-8.
#
#   track 1  INDEX 00 at 0, INDEX 01 at 3 s: a hidden track before the
#            first one. The track starts at 3.
#   track 2  INDEX 00 at 8, INDEX 01 at 10: a two-second pregap. The
#            track starts at 10 and opens with two beeps; 8-10 belongs
#            to track 1 when playing straight through.
#   track 3  INDEX 01 at 00:25:37 -- CD frames are 1/75 s, so 25.493 s.
#   track 4  INDEX 01 at 00:39:74, one frame before the four beeps, and
#            an INDEX 02 at 45 that must not start a track.
$F -c:a flac "$OUT/23 cue-pregap-htoa.flac"
{
    printf 'PERFORMER "Caf\351 Tones"\n'
    printf 'TITLE "Gaps"\n'
    printf 'FILE "23 cue-pregap-htoa.flac" WAVE\n'
    printf '  TRACK 01 AUDIO\n'
    printf '    TITLE "After the Hidden Track"\n'
    printf '    INDEX 00 00:00:00\n'
    printf '    INDEX 01 00:03:00\n'
    printf '  TRACK 02 AUDIO\n'
    printf '    TITLE "Two Beeps After a Pregap"\n'
    printf '    INDEX 00 00:08:00\n'
    printf '    INDEX 01 00:10:00\n'
    printf '  TRACK 03 AUDIO\n'
    printf '    TITLE "Off the Second"\n'
    printf '    INDEX 01 00:25:37\n'
    printf '  TRACK 04 AUDIO\n'
    printf '    TITLE "One Frame Before Four"\n'
    printf '    INDEX 01 00:39:74\n'
    printf '    INDEX 02 00:45:00\n'
} > "$OUT/23 cue-pregap-htoa.cue"

# 24 -- the sheet names a file that is not there. EAC writes
# FILE "CDImage.wav" or "Range.wav", and the image is then renamed,
# compressed to FLAC, or both. The audio beside it with the sheet's own
# base name is the one it means. Tracks at 0, 20 and 40: one, three and
# five beeps.
$F -c:a flac "$OUT/24 cue-filename-mismatch.flac"
{
    printf 'REM COMMENT "ExactAudioCopy v1.6"\n'
    printf 'TITLE "Renamed"\n'
    printf 'FILE "CDImage.wav" WAVE\n'
    printf '  TRACK 01 AUDIO\n'
    printf '    TITLE "One"\n'
    printf '    INDEX 01 00:00:00\n'
    printf '  TRACK 02 AUDIO\n'
    printf '    TITLE "Three"\n'
    printf '    INDEX 01 00:20:00\n'
    printf '  TRACK 03 AUDIO\n'
    printf '    TITLE "Five"\n'
    printf '    INDEX 01 00:40:00\n'
} > "$OUT/24 cue-filename-mismatch.cue"
crlf "$OUT/24 cue-filename-mismatch.cue"

# 25 -- one FILE per track: the minute cut at 20 and 40 into three
# files. Track 3's pregap is the last two seconds of the SECOND file
# (INDEX 00 under 25b, INDEX 01 at the top of 25c), which is EAC's
# "gaps appended to previous tracks" layout and the reason a track's
# start and its file cannot be assumed to be the same thing.
# The three pieces are also ordinary files and list as such.
ffmpeg -loglevel error -y -i landmark.wav -t 20 -c:a flac \
    "$OUT/25a cue-multifile.flac"
ffmpeg -loglevel error -y -ss 20 -i landmark.wav -t 20 -c:a flac \
    "$OUT/25b cue-multifile.flac"
ffmpeg -loglevel error -y -ss 40 -i landmark.wav -c:a flac \
    "$OUT/25c cue-multifile.flac"
{
    printf 'TITLE "Three Files"\n'
    printf 'FILE "25a cue-multifile.flac" WAVE\n'
    printf '  TRACK 01 AUDIO\n'
    printf '    TITLE "One"\n'
    printf '    INDEX 01 00:00:00\n'
    printf 'FILE "25b cue-multifile.flac" WAVE\n'
    printf '  TRACK 02 AUDIO\n'
    printf '    TITLE "Three"\n'
    printf '    INDEX 01 00:00:00\n'
    printf '  TRACK 03 AUDIO\n'
    printf '    TITLE "Five"\n'
    printf '    INDEX 00 00:18:00\n'
    printf 'FILE "25c cue-multifile.flac" WAVE\n'
    printf '    INDEX 01 00:00:00\n'
} > "$OUT/25 cue-multifile.cue"
crlf "$OUT/25 cue-multifile.cue"

# 26 -- broken on purpose, against 22's audio so it costs no new file.
# Of nine tracks three are playable: 01 (0-20), 04 (20-50, and a
# 300-character title) and 09 (50-60). Each other one is wrong in its
# own way -- no INDEX 01, frame 75, going backwards, a data track, a
# start past the end of the audio -- plus an unclosed quote, a line
# that means nothing, and no newline on the last line.
{
    printf 'REM this sheet is broken on purpose\n'
    printf 'TITLE "Unclosed quote\n'
    printf 'FILE "22 cue-flac-image.flac" WAVE\n'
    printf '  TRACK 01 AUDIO\n'
    printf '    TITLE "Fine"\n'
    printf '    INDEX 01 00:00:00\n'
    printf '  TRACK 02 AUDIO\n'
    printf '    TITLE "No INDEX 01"\n'
    printf '    INDEX 00 00:05:00\n'
    printf '  TRACK 03 AUDIO\n'
    printf '    TITLE "Frame 75 Is Not a Frame"\n'
    printf '    INDEX 01 00:15:75\n'
    printf '  TRACK 04 AUDIO\n'
    printf '    TITLE "%s"\n' "$(printf '%0300d' 0 | tr 0 x)"
    printf '    INDEX 01 00:20:00\n'
    printf '  TRACK 06 AUDIO\n'
    printf '    TITLE "Goes Backwards"\n'
    printf '    INDEX 01 00:18:00\n'
    printf '  TRACK 07 MODE1/2352\n'
    printf '    INDEX 01 00:30:00\n'
    printf 'GARBAGE LINE WITH NO MEANING\n'
    printf '  TRACK 08 AUDIO\n'
    printf '    TITLE "Past the End"\n'
    printf '    INDEX 01 01:10:00\n'
    printf '  TRACK 09 AUDIO\n'
    printf '    TITLE "Fine Again"\n'
    printf '    INDEX 01 00:50:00'
} > "$OUT/26 cue-malformed.cue"

# 27 -- made to be looked at, not listened to. bumps.py's six rising
# swells, cut 3/2/1: tracks at 0, 30 and 50 s, so each track's seek bar
# draws three bumps, then two, then one, each set rising. A track that
# draws all six is being measured over the whole image.
[ -f bumps.wav ] || python3 bumps.py
ffmpeg -loglevel error -y -i bumps.wav -c:a flac "$OUT/27 cue-bumps.flac"
{
    printf 'PERFORMER "Bump Ensemble"\n'
    printf 'TITLE "Three Two One"\n'
    printf 'FILE "27 cue-bumps.flac" WAVE\n'
    printf '  TRACK 01 AUDIO\n'
    printf '    TITLE "Three Bumps"\n'
    printf '    INDEX 01 00:00:00\n'
    printf '  TRACK 02 AUDIO\n'
    printf '    TITLE "Two Bumps"\n'
    printf '    INDEX 01 00:30:00\n'
    printf '  TRACK 03 AUDIO\n'
    printf '    TITLE "One Bump"\n'
    printf '    INDEX 01 00:50:00\n'
} > "$OUT/27 cue-bumps.cue"

echo "built into $OUT"
