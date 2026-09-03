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
| 08 aac-adts | cbrseek's ADTS refusal, then 0805's header walk | bar empty for a second or two, then fills and seeks -- on the first play |
| 09 m4a-aac | 0707 sample table and ADTS remux | seeks, exact |
| 10 m4a-alac | 0808's frame-at-a-time path | plays, seeks, within a frame (93 ms) |
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

## 08 seeks during the first play now, and 10 seeks at last

**08 is the file with no seek mechanism at all.** ffmpeg's AAC encoder
writes `buffer_fullness = 0x7FF` in every ADTS header, which is the
stream declaring itself variable bitrate, and `cbrseek.c` refuses those
rather than pretending the byte rate is a line. That refusal is still
the first thing this file tests, and the log still opens with `adts
declares VBR`.

What happens next has changed twice. 0716 had a complete uninterrupted
play record a table as it went -- one play to learn it, a second to use
it, and a drag during the first threw the learning away. 0805 walks the
frame headers instead: every ADTS header states its own length, so the
file can be chained header to header without decoding anything, and the
walk runs on media_task behind the music.

So on a first, never-before-played 08 the bar starts empty and fills a
second or two in, and the drag works from that moment. The log says
which stage it is at -- `no seek mechanism; walking the headers for
one`, then `walked 979 KB in N ms, 30 entries every 2 s`, then
`duration from the header walk: 60 s`. On every play after, the table
comes from the sidecar at the open: `adts: recorded table, 30 entries
every 2 s`.

The recording path is still armed underneath. If the walk finds nothing
usable, or the card is too slow, or the track is skipped before it
finishes, a complete uninterrupted play still writes the table down the
old way -- which is why that code stayed rather than being replaced.

**10 seeks since 0808, by a different route than 09.** ALAC has no ADTS
framing, so there is nothing to remux it into, and 0707 left it on
esp_audio_codec's M4A path -- playing, unseekable. What changed is not
the framing but who does it: `ESP_AUDIO_SIMPLE_DEC_TYPE_ALAC` decodes
ALAC when handed exactly one frame per call, and the sample table 0707
already reads says where every frame begins and ends. The table is the
framing layer.

So the samples go over as they are, one per call, and the seek is the
same table lookup 09 uses. It lands within one frame, which for ALAC is
4096 samples -- 93 ms, against 23 ms for 09, because a bigger frame is
a bigger step.

**Two things send it back to the old path, and both say so.** The
decoder is opened with the magic cookie out of the `alac` box, and
Espressif's header does not say whether it wants the whole atom or the
24-byte config inside it, so both are offered in that order -- `alac:
opened on the config, not the atom` means the second one won. And a
frame decoder needs a buffer holding the largest sample in the file, so
a failed allocation gives the file back too. Either way the fallback is
what shipped before: plays, does not seek.

## One note on 10

esp_audio_codec's M4A parser says `Not support mdat before moov` and
refuses any MP4 that was not written with the index at the front, which
is ffmpeg's default. This file is muxed with `-movflags +faststart`.

That restriction now only bites on the fallback. **09 and 10 both play
either way** while the table path is in use, because it reads the
tables itself and does not care where `moov` sits -- but if 10 drops
back to the M4A parser for either reason above, `+faststart` is what
keeps it playing at all.

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

Since 0809 the log says where a seek landed in hundredths of a second,
which is what makes the rest of this list checkable rather than folk
knowledge. Measured on the host over this corpus: MP4 lands within
0.09 s of the ask, TS within 0.13, and Ogg within 0.99 -- and the
mechanisms had always been that close. Whole seconds were hiding it.

- **Ogg lands within one page.** ffmpeg writes Vorbis and Opus pages of
  roughly a second, and a page is the smallest thing a granule position
  can address. Early in 06 there is one page covering about two
  seconds, so a drag to 0:03 can land at 0:01.
- **FLAC never lands on the very last frame** (93 ms), because a frame
  header is only trusted when the following header confirms it, and the
  last one has no successor.
- **11's length is the span between timestamps, not the file.** A
  transport stream has no stated duration, and the last presentation
  timestamp is the *start* of the last packet, so the span is always
  one packet short. It read 59 s until 0807 rounded the division; it
  now reads 60, but for a reason one packet away from being a
  coincidence, and a stream whose last packet is longer could read
  short again.

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
