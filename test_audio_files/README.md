# Seek test suite

Fourteen files, one minute of the same audio, encoded to exercise every
seek mechanism added in 0700-0708. Copy the folder to a card and drag
the bar.

## What you are listening to

Both channels carry a landmark, so a seek can be judged by ear without
looking at anything:

- **Left** is a tone that steps up in pitch once a second, about two
  octaves over the minute. Pitch alone tells you roughly where you are.
- **Right** ticks quietly on every second boundary, and at each ten
  second mark plays *N* short beeps: one beep at 0:00, two at 0:10,
  three at 0:20, and so on to six at 0:50.

So: drag to just before a ten second mark, and the number of beeps that
follows tells you whether the seek landed where the bar said. A seek
that is one second early or late is audible as a tick in the wrong
place; one that is wrong by more is obvious from the beep count.

## The files

| File | Exercises | Expected |
| --- | --- | --- |
| 01 wav-pcm16 | cbrseek, WAV | seeks, exact |
| 02 wav-pcm24 | cbrseek, non-2-byte block align | seeks, exact |
| 03 mp3-cbr-noxing | 0703 first play: scan at open, table harvested | seeks; slow open the first time, fast after |
| 04 mp3-vbr-xing | 0703 other path: no scan, lazy index on first drag | seeks; first drag slow, later drags fast |
| 05 flac | 0704 bisection | seeks, within a frame |
| 06 ogg-vorbis | 0705, multi-page codebooks in the preamble | seeks, within a page |
| 07 opus | 0705, 48 kHz granule and pre-skip | seeks, within a page |
| 08 aac-adts | cbrseek's ADTS refusal, then 0716's harvested table | no seek on the first two plays; seeks after the third |
| 09 m4a-aac | 0707 sample table and ADTS remux | seeks, exact |
| 10 m4a-alac | 0707's deliberate fallback | **plays, does not seek** (needs `+faststart`) |
| 11 ts-aac | 0706 packet lattice | seeks, within a frame |
| 12 mp3-bigart-noxing | ID3v2 with a 600x600 cover ahead of the audio | seeks; art shows |
| 13 flac-short-3s | a track shorter than the bar is wide | seeks without falling over |
| 14 wav-mono-22k | mono, half rate | seeks, exact |

## 08 takes three plays, and 10 never gets there

**08 is now the file that has to be listened to before it can be
seeked.** ffmpeg's AAC encoder writes `buffer_fullness = 0x7FF` in every
ADTS header, which is the stream declaring itself variable bitrate, and
`cbrseek.c` refuses those rather than pretending the byte rate is a
line. That refusal is still the first thing this file tests. What
changed is what happens next: 0716 gives a stream with no other
mechanism a table recorded from a play that reached the end without a
seek in it, and 0717 fixed the guard that stopped any pair from ever
being appended.

So the file needs three complete, uninterrupted plays: one for the
loudness, one for the length, one for the table. Drag before that and
the press is correctly ignored. The log says which stage it is at --
`adts declares VBR`, then `no seek mechanism; recording a table as it
plays`, then `table recorded, N entries every 2 s`. A drag during any
of those three plays throws that play's measurement away and it starts
again next time, which is easy to do by accident when testing.

**10 does not seek at all.** ALAC has no ADTS framing, so 0707 cannot
remux it and leaves it on esp_audio_codec's own M4A path -- playing,
unseekable, exactly as before. If it seeks, something has gone wrong.

## One note on 10

esp_audio_codec's M4A parser says `Not support mdat before moov` and
refuses any MP4 that was not written with the index at the front, which
is ffmpeg's default. This file is muxed with `-movflags +faststart` so
it plays.

Worth knowing which way that cuts: **09 plays either way**, because
0707 reads the tables itself and does not care where `moov` sits. The
restriction belongs to the parser 0707 stopped using, and 10 is the
file that still has to live with it.

## Known imprecision, so it is not reported as a bug

- **Ogg lands within one page.** ffmpeg writes Vorbis and Opus pages of
  roughly a second, and a page is the smallest thing a granule position
  can address. Early in 06 there is one page covering about two
  seconds, so a drag to 0:03 can land at 0:01.
- **FLAC never lands on the very last frame** (93 ms), because a frame
  header is only trusted when the following header confirms it, and the
  last one has no successor.
- **11 reports 59 s rather than 60.** A transport stream has no stated
  duration; it is the span between the first and last presentation
  timestamps, and the last timestamp is the *start* of the last packet.

## Generated with

`build/gen.py` writes `landmark.wav` and nothing else: sixty seconds of
stepping tone and beeps, in Python, with no dependencies. Every file in
this folder is that one minute put through ffmpeg 6.1.1, and those
commands are in **`build/encode.sh`** -- which is new, because until now
they existed only as this paragraph saying the word "ffmpeg".

    cd build && ./encode.sh out

rebuilds the folder into `build/out`. Against ffmpeg 6.1.1 every file
comes back byte-identical except 06 and 07, which carry a random Ogg
serial number, and 12, whose cover image was not kept -- the script
generates one with `testsrc2` instead, so the file it builds is the
same test with a different picture in it.

Everything here is synthesised -- no copyright, nothing to attribute.
