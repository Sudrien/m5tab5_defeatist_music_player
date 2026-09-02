# Seek test suite

Twenty-one files, one minute of the same audio, encoded to exercise
every seek mechanism added in 0700-0717 and the sample-width handling
added in 0803. Copy the folder to a card and drag
the bar.

The first fourteen cover the mechanisms as they shipped. 15 to 18 are
the cases none of them reached: a branch with no file, and three shapes
of input that the probes are supposed to refuse and that nothing ever
handed them.

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
| 15 aac-adts-cbr | cbrseek's ADTS branch, which 08 never reaches; fdk-aac, not ffmpeg | seeks from the first play, within a frame |
| 16 m4a-aac-fragmented | mp4seek with no `stbl` to read | **plays or refuses; must not seek** |
| 17 ts-aac-spliced | tsseek's non-monotonic PTS refusal | **no seek, no duration, empty bar** |
| 18 ogg-vorbis-chained | oggseek and duration.c where the tail is another stream | 20 s on the bar, seeks within it, clamps at the chain |
| 19 wav-pcm32 | 32-bit integer | **refuses, and says 32-bit** |
| 20 wav-float32 | 32-bit float, indistinguishable from 19 at the decoder | **refuses, identically** |
| 21 flac-24bit | the fold on a real decoder rather than a PCM chunk | plays, seeks |

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

So the file needs two complete, uninterrupted plays, not three: the
loudness, the length and the table are all harvested from the same
pass, and the second play is the one that uses them. Drag before that and
the press is correctly ignored. The log says which stage it is at -- `adts declares VBR` and `no seek
mechanism; recording a table as it plays` on the first, then `table
recorded, N entries every 2 s` when it ends, and `adts: recorded table,
N entries every 2 s` at the open of every play after. A drag during the
recording play throws that play's measurement away and it starts again
next time, which is easy to do by accident when testing.

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

## The four added for the gaps

**15 is the only positive one, and it took two attempts.** `cbrseek.c`
has an ADTS branch that maps time to offset from a steady frame rate,
and no file in the folder had ever run it -- 08 declares
`buffer_fullness = 0x7FF` and stops the probe at the first window,
which is 08's entire purpose and cannot also be this one's.

0801 built 15 by padding every frame of an ffmpeg encode to a constant
length. On the board it decoded one block and died: `Failed to decode
aac frame, error:30`. The padding is zero bytes sitting after the raw
data block's terminator, and to an AAC decoder a zero byte is not
padding, it is `ID_SCE` -- a single channel element. ffmpeg stops at
the terminator and never sees them, which is how the file passed a
byte-for-byte decode check on the host and failed on the only decoder
that mattered.

15 is now fdk-aac output: a real CBR encode with real fullness values.
It needs a two-minute build from source, which `build/encode.sh`
documents and skips gracefully without.

**And the file it replaced was hiding a second fault.** A genuine CBR
encoder holds a constant *average* rate and borrows bits between
frames, so the real file spread 3.42% across five 4 KB windows and
`cbrseek.c` refused it -- meaning the branch would have turned away
every real CBR AAC file ever handed to it, while passing the synthetic
one whose frames were all the same size by construction. 0804 widened
the probe window to 16 KB, where the same file spreads 0.28%. The
resync windows stayed at 4 KB; they only need to find one header.

**16 and 17 are refusals, and a refusal that stops refusing is a
regression.** 16 keeps its sample tables in `moof` boxes, so
`mp4_probe()` finds no `stbl` and the file falls back to
esp_audio_codec's M4A parser, which may or may not play it -- either is
fine, seeking is not. 17 is spliced from two sources so its PTS
restarts part way; ffprobe reads it as 91873 seconds long, which is
exactly what a bisection over a non-monotonic key would be searching.
`ts_seek_probe()` should refuse it and the bar should stay empty.

**18 had no expected answer when it was added, and 0802 is that
answer.** A chained Ogg is two logical streams end to end and is
perfectly legal -- `cat a.ogg b.ogg` makes one. The parser in the
archive compares each page's serial against the one it learned at the
start and drops the rest, so the first stream is the audible part, and
that is what both the bar and the drag are now bounded to.

Both faults the file was made to look for were real. `oggseek.c` left
`last_granule` at 0 because the tail window held nothing of ours, and
zero is not "no clamp", it is a clamp that never fires -- a drag past
20 s restarted the track. `duration.c` read the granule off the last
page in the file without checking whose it was, and reported the second
stream's length.

**The halves are 20 and 40 seconds, and that is load-bearing.** They
were 30 and 30, which made the duration bug invisible: the wrong page
gave 30 s and the right page gives 30 s, so the file certified the bug
as passing. A test whose wrong answer coincides with its right one is
not a test. 0802 regenerated it unequal, and the two answers are now 20
and 40.

## 24 folds, 32 does not

Before 0803 file 02 did not play at all: `wav is 24-bit, this player is
16-bit only`, zero blocks, skipped. It still probed correctly -- the
byte rate and the seekability were right -- so the file was testing
cbrseek's arithmetic and nothing else, and the README implied it was
playing when it never had.

The fold rounds each 24-bit sample to 16 at the one point where the
samples exist and have not yet become the player's -- the ring, the
gain, the crossfade, the envelope and the I2S slots are all 16-bit and
none of them learn anything changed. 02 is the proof: it is 01's audio
at a greater width, so folding it must reproduce 01 exactly, and on the
host it does, for all 5292000 samples with no sample differing by so
much as one LSB.

**19 and 20 are why 32 stays refused.** They are the same ten seconds
as integer and as float, and `esp_audio_simple_dec_info_t` reports a
bit count with nothing to say which is which. Folding float samples as
integers sends full-scale noise to headphones, so there is no side to
guess on. Both must refuse and the log must name the width; if either
one ever plays, something has started guessing.

**21 is the case worth having.** 02 arrives as PCM in a chunk, where
being 24-bit is a fact about the file. 21 arrives from the FLAC
decoder, where it is a fact about the decoder's output, which is where
this matters in real use -- 24-bit FLAC is most of what a bought
download is.

## Not covered, and why

- **AMR.** `cbrseek.c` claims fixed-mode AMR and nothing exercises it,
  because no stock ffmpeg has an AMR encoder -- it needs a build with
  libopencore-amrnb. A synthetic file with valid headers and junk
  payload would test the probe and be useless to listen to, which is
  the wrong trade for this folder.
- **The MP4 sample-table ceiling** (`MP4_MAX_SAMPLES`, 200000, about 77
  minutes) and **the MP3 table's doubling past 256 entries** both need
  files far longer than anything here. Lower the constant in a
  throwaway build instead; that tests the same branch in one minute.
- **Protected AAC.** Cannot be generated, and the failure it produces
  is the decoder's, not the seek path's.

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
