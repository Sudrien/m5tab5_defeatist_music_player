# CLAUDE.md

Notes for anyone (or anything) working on this repository. The README is
the design document; this file is the set of things that are easy to get

## Why two decoders

`main/decoder.c` routes by file extension:

| Extension | Backend | Why |
| --- | --- | --- |
| `.mp3` | minimp3 | Layers I, II **and** III, free format, and `minimp3_ex` already parses Xing/LAME for gapless trim |
| `.flac` `.wav` `.m4a` `.aac` `.ogg` `.opus` `.ts` `.amr` | esp_audio_codec | One component, one API, Espressif-maintained |

esp_audio_codec has an MP3 decoder too, and it turns out to be minimp3 as
well -- the same upstream, built into `libesp_audio_codec.a` with the
same public symbol names. `mp3dec_init` and `mp3dec_decode_frame`
therefore collide at link time with our vendored copy.

`CONFIG_AUDIO_DECODER_MP3_SUPPORT=n` does **not** fix this, though it is
set anyway for tidiness. esp_audio_codec ships as a precompiled archive
(`lib/esp32p4/libesp_audio_codec.a`); the Kconfig option gates the
registration source that gets built here, not the prebuilt
`esp_mp3_dec.c.obj` sitting in the archive. The duplicate symbols are
already in the blob and nothing on this side can remove them.

The fix is `components/minimp3/minimp3_prefix.h`, included before
`minimp3_ex.h`, which renames our copy to `tab5_mp3dec_*`. Theirs stays
in the binary, unreferenced.

Keeping ours rather than theirs is deliberate: `minimp3_ex` gives Layers
I/II, free format and gapless trim, and the bundled build exposes none of
that through the simple-decoder API.

If `multiple definition of mp3dec_init` comes back after a minimp3
update, upstream has added an exported function -- diff the declarations
at the top of `minimp3.h` and `minimp3_ex.h` against the list in
`minimp3_prefix.h`.

This is the point of divergence from `m5tab5_mp3_example`, which used
libhelix directly and inherited its Layer III-only limit.

## What this fixes that the example did not

The example hand-rolled ID3v2 skipping, ID3v1 trimming and sync-word
scanning in `mp3_audio_extent()` and `play_mp3()`. All of it is gone,
along with four bugs that were in it:

- ID3v2 footer flag (`0x10`) was not accounted for, so ten stray bytes
  reached the sync scanner.
- APEv2 and Lyrics3 trailers were not trimmed the way ID3v1 was.
- The Xing/Info header frame was decoded and queued as ~26 ms of silence.
- Encoder delay and padding were never trimmed, so nothing was gapless.

minimp3_ex handles all four. The esp_audio_codec parsers handle the
equivalents for their own containers.

## Things to check before trusting this

Written against the documented shape of the esp_audio_codec API rather
than a compiler. Verify against the version the registry actually
resolves:

- The component version is pinned `>=2.3.0,<2.6.0`. The ceiling is
  hardware, not caution: v2.6 requires ESP32-P4 rev >= 3.0 and the Tab5
  is rev v1.3, which is the same fact `sdkconfig.defaults` states as
  `CONFIG_ESP32P4_SELECTS_REV_LESS_V3`. Do not unpin to latest. If a
  format is only in 2.6+, vendor a decoder next to minimp3 instead.
- `DECODER_MAX_INT16` is a guess at the worst-case frame across all
  backends. If a FLAC with a 4608-sample block or an AAC-Plus file logs
  "frame needs N bytes", raise it rather than truncating.
- ~~Only 16-bit output is handled; `esp_codec_read()` rejects anything
  else rather than playing it as noise. A 24-bit FLAC will refuse to
  play.~~ Stale since 0803, corrected by 1002. **24-bit folds to 16 and
  plays**; the samples are already in a buffer of ours one call before
  they belong to anything else, so rounding them there costs a pass over
  the frame and changes nothing downstream. **32-bit stays refused**, and
  deliberately: `esp_audio_simple_dec_info_t` reports a bit count with no
  way to tell an integer stream from a float one, and folding float as
  integer is full-scale noise into headphones. Files 02 and 21 of the
  corpus are the 24-bit cases and both play.

## One-frame-per-call codecs

The header divides esp_audio_codec's decoders into those that "support
input data of any size" and those that "only support input data with a
size of one encoded frame". The sliding window `esp_codec_read()` feeds
from the file cannot promise a frame, so the second group is excluded
from the streamed path: `_RAW_OPUS`, `_ALAC`, `_VORBIS`, `_ADPCM`,
`_LC3`, `_SBC` and `_G722`.

**`_ALAC` is the exception, since 0808.** The restriction is about
reading a file as a stream, and an MP4 is not read as a stream here: the
sample table says where every frame begins and ends, so the frames can
be handed over one at a time exactly as the decoder asks. The framing
layer that group is missing is the table. Nothing else in the list has
one, which is why nothing else moved.

`.opus` and `.ogg` therefore both route to `_OGG`, the container parser,
which does take arbitrary lengths. That is also the better mapping in
practice: raw headerless Opus is rare on disk and Ogg-encapsulated Opus
is what everything actually ships.

## Vendored headers are fetched, not committed

minimp3 and pngle are pulled by `cmake/vendored.cmake` during
`idf.py build`, pinned to commit SHAs with a SHA256 per file. Nothing to
run first.

The pin is load-bearing rather than tidiness: `minimp3_prefix.h` lists
every symbol minimp3 exports, and an unpinned upstream that adds one
brings the link collision back for whoever clones next but not for you.
`tools/fetch_vendored.sh` carries the same pins for offline use.

This is the opposite call from `components/fatfs/`, which
`cmake/exfat.cmake` writes -- by running `tools/enable_exfat.sh` at
configure time, the same way this file fetches -- and which is also not
committed. But that one has no pin, because it is patched from whatever
IDF you have installed, which is the thing a pin would have to name and
cannot.

## On-screen controls

`ui.c` draws the transport into the panel's scan buffer directly, the way
`albumart.c` already does. No LVGL, no M5Canvas -- a toolkit would be more
code than the thing it draws.

Eight rows, top to bottom, each one thing:

| Row | What |
| --- | --- |
| 1 | cover art, 720x720 |
| 2 | the seek bar -- the loudness envelope, full panel width |
| 3 | elapsed left-justified, remaining right-justified |
| 4 | title, bouncing when it does not fit |
| 5 | album |
| 6 | artist |
| 7 | folder \| prev, play/pause, next \| sleep |
| 8 | volume |

**The artwork is a square and the bar is the remainder**, which is the
reverse of every version before it. The bar used to be sized to its
contents and the cover got what was left, so a 720 px wide column was 964
rows tall and `albumart.c` letterboxed a square cover into the middle of
it with 122 rows of black above and below. Those rows were not doing
anything. `UI_ART_H` is 720 and `UI_BAR_H` is `1280 - UI_ART_H`; the
controls went from 356 px to 560 and the cover lost nothing it was using.

**Reading down is reading outward.** What is playing, where in it, what it
is called, and then the controls -- which are the only rows a finger goes
near, and are therefore the ones nearest the hand. The previous layout put
the three text rows at the top of the bar and the seek bar under them,
which meant the two things that move while a track plays were the two
furthest from the artwork they belong to.

**The clocks got a row of their own** because the envelope took the full
width. They used to flank it, which is what `SEEK_X0`'s 142 px of margin
at each end was for: 284 px of a 720 px panel spent on two five-character
numbers, taken out of the middle of the one element that wants width.
`SEEK_X0` is 0 now.

**The right-hand clock counts down, not up.** The total was the same five
characters for the whole song and said nothing the bar was not already
showing. How long is left is the question people actually ask of a player,
and it is the one number on screen that the seek bar cannot answer by
being looked at. It is drawn with a leading minus -- `gfx_draw_time_neg()`
rather than a flag, because the sign changes the width and the caller
right-justifies the run.

**Album above artist**, which is also a reversal. Downward the rows now go
most specific to least: this track, the record it is on, the person who
made it.

Played portion of the seek bar and the set portion of volume are both red;
the remainder of each is grey. Volume applies live during a drag, because
you want to hear it. Seek fires once on release -- re-decoding on every
poll would thrash the card.

**A seek drag turns row 3 red and puts the target in it.** Both clocks,
together, so they still add up to the length. That is the whole feedback:
the digits are already the right size, in the right place, and are the
number a seek is adjusting. The colour is what makes it honest -- same
place and same size as the playing clock, so without it a dragged clock
reads as a seek that already happened, seconds before the decode loop has
been asked. Red is requested, grey is playing, which is the distinction
the envelope one row up already draws in the same two colours. A volume
drag leaves row 3 alone: it is not a position, and the slider's own fill
plus the sound in your ears is the readout.

Hit targets are padded well past the drawn shapes (`HIT_PAD_X`,
`HIT_PAD_Y`). Row 7's five centres are spaced so the padded boxes do not
touch: play claims `BTN_R + HIT_PAD_X` = 60 either side and the skips 38,
so at 248, 360 and 472 there is clear air between them. Buttons are still
tested before sliders, but they no longer *need* to be -- boxes that
overlap and are disambiguated by test order work right up until the order
changes.

With the screen off, a touch only wakes -- it does not also press whatever
was under it, or one tap would turn the screen straight back off.

### Prev is two buttons

Row 7's back button seeks to the start of the track, and only skips to the
previous track if it is already there. The threshold is three seconds,
which is roughly how long it takes to decide you meant the other one.
Every physical transport has worked this way, and the reason is that
"restart this" and "go back one" are both wanted from the same button far
more often than either is wanted from its own.

The forward button asks `playlist_next()` with `PLAY_ORDER_ONE` mapped to
`PLAY_ORDER_ALL`. "One" is an answer about what happens at the *end of a
track*, and a button press is not the end of a track; passing the order
through unchanged returns NULL and the button does nothing, which reads as
broken rather than as a setting being respected.

Neither button is a playlist history. Shuffle's back button goes to the
previous *index*, not the previously played track, for the reason
`playlist_prev()` already gives: undoing a random choice needs a stack,
and the button is there to skip back one track.

## The title bounces

A title too long for 720 px slides left, pauses, slides back, pauses, and
repeats. Album and artist are still cut with an ellipsis.

The asymmetry is deliberate. The title is the one string on screen that is
not interchangeable with something else already visible -- an album can be
truncated because the cover above it says the same thing, and an artist
because the album implies it, but `Everything In Its Right Pl...` is a
song nobody can name.

- **Bounce rather than wrap.** A wrapping marquee needs the string drawn
  twice with a separator and never shows the beginning and end together. A
  bounce shows the head, travels, shows the tail, and comes back. On
  titles, where the front identifies the song and the back is usually
  `(Remastered 2011)`, the head is worth returning to.
- **It needs a clipped text primitive, not the existing one.**
  `gfx_draw_text()` truncates at a character boundary and adds dots, which
  is right for a list of filenames and wrong for a string sliding past a
  fixed opening, where a glyph has to be drawn half in and half out.
  `gfx_draw_text_clipped()` clips the fill runs rather than the glyphs,
  and takes an x that may sit outside the window on either side.
- **It is stepped from `ui_draw()`, not a timer**, so it moves at whatever
  rate the bar is repainted. That rate was 10 Hz when no finger is down --
  correct for a seconds-resolution clock, and visibly wrong for something
  moving, where it reads as a title jumping three pixels at a time. So
  `ui_animating()` exists and the UI task polls at 25 Hz while it is true.
  A title that fits never sets it, so a short one costs nothing.
- **The title is compared by pointer *and* length.** The player hands the
  UI a pointer into its own tag buffer, which is rewritten in place
  between tracks: the string changes without the pointer changing. Neither
  test is reliable alone and both are cheap. Getting it wrong means a
  short title inheriting the previous long one's offset and being drawn
  off the side of the panel.

## Touch

`touch.c` is a thin wrapper over `esp_lcd_touch`. The probe order and the
INT handling are lifted from `m5tab5_esp_idf_display_example`, where they
are confirmed on hardware -- this file adds only a one-point API so ui.c
does not have to know which controller answered.

Two revisions exist and the touch controller is not named after the panel:

| Revision | Panel | Touch |
| --- | --- | --- |
| rev 1 | ILI9881C | GT911, backup address |
| rev 2 | ST7121 / ST7123 | ST7123 at 0x55 |

So it is probed, ST7123 first then GT911, the same order M5's BSP uses.
Both driver components are in `idf_component.yml` for that reason.

**The GT911 path drives GPIO 23 low.** There is a pull-up to 3V3 on the
INT line on rev 1 that otherwise stops the controller responding, so that
path holds INT low and polls instead of using the interrupt. Driving it
high is the intuitive thing and it is wrong.

`TP_RST` needs no handling here: it is expander 1 `P5`, already driven
high by the `PI4IOE1_OUT_SET` value the display path needs. `touch_init()`
does have to run after `io_expanders_init()`, and waits 200 ms after it,
because the controller needs a moment once reset is released.

### Sizes are set for 294 PPI

720x1280 on a 5" panel is about 294 PPI, so a 12 px font glyph is 1.0 mm
tall -- unreadable at arm's length. Everything is scaled for that rather
than left at values that looked right in a 96 PPI mockup: the title is
ark12 at scale 3 (36 px, about 3.1 mm, roughly a phone's body text),
album and artist at scale 3, the MM:SS digits 20x38, the slider thumbs
16 px radius, and the bar itself 560 px.

Two font changes have moved these numbers. font8x8 was a 9 px advance per
glyph and 8 px tall; ark10 was 6 and 10; ark12 is 7 and 12 for halfwidth
glyphs and 13 and 12 for fullwidth ones. The vertical growth is why
`GFX_GLYPH_H(scale)` exists -- `browser.c` was centring rows with a
literal `8 * scale`, which was right for font8x8 and silently wrong for
both successors.

**The scale numbers dropped when the cell grew, to hold physical size.**
The title was ark10 at scale 4 (40 px); it is ark12 at scale 3 (36 px).
The album-art headline went 6 to 5 (60 px either way, exactly). That is
the trade the 12 px cut buys: nearly the same millimetres on the panel,
but each drawn pixel is a 3x3 block rather than 4x4, over a glyph with
more detail drawn into it in the first place. Everything else already fit
its box at the taller cell and was left alone -- checked against `ROW_H`
(88), `PATH_H` (60) and `TAB_H` (96) rather than assumed.

One trap worth naming, because it was live for a few minutes: the marquee
measures with `gfx_text_w(title, N)` and draws with `gfx_draw_text_clipped(
..., N, ...)`, and **N has to be the same in both**. Changing the draw
scale alone leaves the bounce distance computed for a width the string no
longer has.

If the layout is ever moved again, two things are load-bearing rather than
aesthetic:

- Row 7's centres have to keep the padded hit boxes apart. See above.

`BUBBLE_ABOVE` used to be the other entry here. Nothing `ui_task` draws
reaches above `s_bar_top` any more, and that is worth keeping true --
anything that breaks it needs its own erase path and its own blit.

## Text rows

Title, album and artist, a row each. Album and artist are clipped with an
ellipsis rather than wrapped; the title bounces, for the reasons above.

Artist and album used to share a line, on the grounds that three stacked
rows made the bar taller than the artwork could spare. That was the wrong
trade twice over -- and the second time it was not even true, because the
artwork was not using the rows. The joined string was built in a 96 byte
buffer from two 64 byte tag fields, so anything approaching full length
was silently truncated, and it happened to truncate the album only because
of the argument order. An album title of any real length pushed the artist
out of the row entirely, which is the one part people actually read.

Album is dimmer than artist. Three rows of equal weight read as a block of
text; the hierarchy is what makes it scannable at arm's length.

`id3_read_tags()` lives in `albumart.c` rather than its own file because
the frame walker it needs is the one `albumart_extract()` already has --
version-dependent frame sizes, the padding check, the tag-end bound. A
second copy is the thing that drifts.

The font is **Ark Pixel Font**, 12px, as `components/ark12` -- 20,669
glyphs across Basic Latin, Latin-1 Supplement, Latin Extended-A, Cyrillic,
Hiragana, Katakana, CJK Symbols and Punctuation, both CJK Unified
Ideograph blocks in range, and Halfwidth and Fullwidth Forms. It replaced
`font8x8`, which was ASCII-only and made every accented artist name a row
of question marks.

It is not vendored the way minimp3 and pngle are, because it cannot be:
Ark ships one PNG per glyph and builds its font files at release time, so
there is no single file for `cmake/vendored.cmake` to pin a SHA256
against. `tools/gen_ark12.py` converts the PNGs to a C table, is run by
hand, and its output is committed. The generated header names the
upstream commit, which is the same guarantee moved somewhere that can
hold it.

### The rate-change drain was never the bug; the log line was (0908)

A board run showed four of these and they look alarming:

    rate change to 22050 Hz; draining 1416 KB first
    first sound 8326 ms after the press (open was 12 ms of it)

Eight seconds to start a track whose open took twelve milliseconds. I
read it as a stall worth fixing and said so. It is not, and this file
already knew: see "Not a fault: the 12.9 s before track 14" above, which
has been correct since 0710.

**Checked rather than argued, because the first argument was wrong.** I
claimed the cost was not proportional to the queued bytes -- 500 KB
costing two thirds of what 1416 KB cost -- which assumed both drained at
44.1 kHz. They did not. The 500 KB case drained after a 22.05 kHz track,
so its byte rate was half. The ring always holds 16-bit stereo, mono
duplicated before it arrives, so it empties at the outgoing rate times
four:

    1416 KB at 44.1 kHz -> 8220 ms predicted, 8326 observed  (1.3%)
     500 KB at 22.05 kHz -> 5805 ms predicted, 5911 observed  (1.8%)

Both inside 2%. The wait is exactly the queued audio playing out, with no
fixed overhead hiding in it, and every millisecond is the previous track
being heard. Capping it would reintroduce the bug the drain was written
to fix -- twenty seconds of Vorbis discarded at a boundary, heard as the
track jumping to its end.

**So the change is to the logging and nothing else.** The rate-change
path now times its own drain and says what it was worth, and the first
sound line subtracts it out and states it separately:

    drained in 8220 ms of audio, not silence: that was the previous
      track finishing
    first sound 8326 ms to sound (open was 12 ms of it, and 8220 ms was
      the previous track playing out), ring 0%

"after the press" is gone too. On an automatic boundary nobody pressed
anything; "to sound" is true either way.

**The lesson is about where the explanation lives.** The analysis above
was right, written down, and five patches old, and the question still got
asked again from a fresh log -- by me -- because the log line did not
carry what the document knew. A document that answers a question nobody
thinks to look up has not answered it. This is the second time this
session that a misleading line cost real time: the 998 ms constant sat in
plain sight through three builds for the same reason.

Two historical excerpts elsewhere in player.c still quote the old
wording. They are marked as predating this patch rather than rewritten,
because they are evidence of what was logged at the time.

### The ALAC warning was about a component that path does not use (0907)

Every ALAC track logged this at open, then played perfectly:

    W AUD_SDEC: Not find default parser for 1128352833

1128352833 is 0x41434C41 -- 'ALAC', read the other way up. The library
is looking for a default *parser*, the component that finds frame
boundaries in a stream, and there is not one for ALAC. There does not
need to be. `use_frame_dec` is true on this path precisely because the
MP4 sample table does the framing and every buffer handed over is
already exactly one frame. The warning reports the absence of a thing
this path deliberately does without.

Left alone it is worse than untidy. A log that cries wolf once per file
of a whole format is a log people stop reading, and the last several
patches were found by reading logs closely -- the 998 ms constant sat in
plain sight through three builds because the surrounding noise was
normal.

`AUD_SDEC` is inside the precompiled esp_audio_codec archive, so there is
nothing to patch at source. The tag's threshold is raised to
`ESP_LOG_ERROR` across the two ALAC opens and put back after.

Narrow deliberately, in three ways:

- **Only that tag.** Everything else logs as before.
- **Only those calls.** The M4A fallback open below is an ordinary open
  with every reason to be heard, and the restore is above it. There is no
  early return between the set and the restore, so the suppression cannot
  leak out of this function.
- **Only below ERROR.** A genuine AUD_SDEC failure still prints. What is
  hidden is one warning that is false on this path.

The previous level is read with `esp_log_level_get()` and restored,
rather than assuming `ESP_LOG_INFO`, so this stays correct under a build
with a different `CONFIG_LOG_DEFAULT_LEVEL`.

`SDEC_TAG` is spelled out in this file because it is not ours and no
header declares it -- it is a string inside the archive, recovered from
the log line. If a future component renames the tag the suppression
stops working and the warning comes back, which is the right direction
for this to fail: noisy rather than silent.

**Checked, having got this wrong recently.** 0902 called
`usb_host_endpoint_halt()` on EP0 on the strength of the call order being
documented, without checking the argument was valid, and it was refused
at runtime. So `esp_log_level_get()` was confirmed to exist in the IDF
5.x API reference before being used, rather than assumed from the shape
of `esp_log_level_set()`.

**Not flashed,** and only partly compiled: `decoder.c` pulls in
`minimp3_ex.h`, which `cmake/vendored.cmake` fetches at build time and is
not in the tree, so the host stubs cannot reach it. The added pattern was
compiled in isolation and the surrounding control flow read by eye. What
to look for: an ALAC track opening with no AUD_SDEC line, and an M4A AAC
track still logging normally.

### The eight-second wait was for a ring nobody was filling (0906)

Two warnings appear in every log this program has ever produced, at
boot, before anything is playing:

    W cover:    ring never reached 60% in 8000 ms; going ahead anyway
    W prefetch: ring never reached 75% in 8000 ms; going ahead anyway

They were read as storage contention for a long time, including by me,
one patch ago. They are not. In a boot log there is nothing for the
reads to contend with -- and in the log that finally made it obvious,
both fired at 10008 and 18699 while playback did not start until 26506.

`media_settle()` waits for the ring to reach a floor. The ring is filled
by a decode loop. With nothing chosen -- the chooser open, a track
resumed but not started -- there is no decode loop, the level sits at 0
and cannot rise, and the wait ran its full eight seconds every time.

`s_ring_pct`'s own -1 escape does not catch it, which is why this
survived: -1 means "no ring yet", and `i2s_writer_task` loops on a 100 ms
receive whether anything is playing or not, so `ring_publish()` has long
since published a real 0.

**The predicate is `!s_decoding && !s_track_changing`,** which is the one
`player_loop()` already idles the amplifier on. Both halves are load
bearing and for different reasons. `s_decoding` covers a running decode.
`s_track_changing` covers the gap *before* one -- and that gap is not
small: `s_decoding` is set only after `decoder_open()` returns, and
`decoder_open()` on a Xing-less MP3 scans the whole file. 1263 ms in the
logs, against a `MEDIA_ART_DELAY_MS` of 700. Testing `s_decoding` alone
would have released the cover read into the middle of an index scan --
the exact contention `media_settle()` exists to prevent, reintroduced by
the fix for it.

The bounded wait keeps its original job. Its comment listed "a slow
device, a very high bitrate" as the cases it was carrying; "nothing is
playing" was not on that list and should never have been on it.

### And the priority the reads were filed under (0906)

`covertag.c` read everything as `STORAGE_IO_PREFETCH`. Right for the
next track, wrong for the current one, and both go through that file:
`load_tags()` runs on the decode loop at the track change and `do_art()`
on media_task behind it -- both about the song being listened to --
while `prefetch_next()` and its neighbours are about a song nobody has
heard yet. The playing track's own tags and cover were queued behind, and
at equal standing with, work for a track that might never be reached.

Worth fixing in the same patch as the wait above rather than after it:
removing the eight seconds makes these reads start sooner, so the
priority they are filed under starts mattering more, not less.

### And `duration.c` was the same wart, one file over (1000)

0906 threaded a class parameter through `covertag.c` and fixed
`albumart.c` alongside it, on the finding that the playing track's own
tags and cover were filed as `STORAGE_IO_PREFETCH` -- queued behind, and
at equal standing with, work for a track nobody has heard yet.
`duration.c` had the identical constant and was not touched.

It is reached from `decoder_duration_sec()`, which `play_file()` calls
on the decode loop immediately after `decoder_open()` -- the pause
before the first sample, which is the one read in the program nothing
may queue behind. So the length of the track being listened to was filed
as speculative work about a different track.

**This was already written down twice and neither note closed it.**
`cbrseek.c` says its class is `PLAYBACK` "not `duration.c`'s PREFETCH"
and calls it "a wart this file declined to copy"; 0102's own section
names the `covertag.c` half in the same words. Both correctly identified
it, both declined to fix it, and the entry in the open list named only
`covertag.c` -- so when 0906 fixed that file, the list read as closed and
the second instance had nothing pointing at it. **A note that records a
fault without owning it is a note that keeps the fault.**

The class is a parameter rather than a constant, for 0906's reason
restated: the answer depends on who is asking, not on what is read.
Every probe below `duration_probe()` takes it, including the Ogg tail
window and the chained-stream `ogg_stream_extent()` fallback, which are
the only reads here big enough to matter -- 64 KB and a couple of dozen
short reads respectively. The rest are a few hundred bytes each and
would not have been worth a patch on their own.

Today's only caller passes `STORAGE_IO_PLAYBACK`. Nothing prefetches a
duration yet; when something does, it passes `PREFETCH` and is queued
behind the music, which is the behaviour the constant was accidentally
giving every caller.

**Not flashed.** `duration.c` compiles clean under
`gcc -Wall -Wextra -Wformat=2` against the host stubs.

**And the check this entry gave was wrong -- see 1001.** It said to look
for the probe's reads in the `open` window. They cannot be there:
`storage_io_report("open")` closes immediately after `decoder_open()`,
and `decoder_duration_sec()` runs some hundreds of lines later. The
right window is `track`, and the board confirmed the fix from it.

### 1000's own falsification condition was in the wrong window (1001)

1000 said to look for the probe's reads under `open playback` rather
than `open prefetch`. That check could never have fired either way.
`storage_io_report("open")` is called at the top of `play_file()`, right
after `open took N ms`; `decoder_duration_sec()` -- the only caller of
`duration_probe()` -- runs much further down the same function. The
board says so plainly, 13 ms apart:

    open took 28 ms
    open playback: 22 reads, 128 KB in 18 ms held of 28 ms (64%)
    tab5_dur: container says 60s

The probe's reads land in the **`track`** window, which is reported when
the track ends.

**Read from the right window, the fix is confirmed.** An Ogg track's own
report, over a window that opens at the probe:

    track playback: 58 reads, 181 KB in 36 ms held of 263151 ms
    track prefetch: 36 reads,   0 KB in 10 ms held, 39 KB/s

`probe_ogg()` reads a 64 KB tail window (`OGG_WINDOW`). PREFETCH
accounts for 0 KB across the whole track -- 10 ms at 39 KB/s is about
390 bytes, which is the sidecar and the tag reads. A 64 KB read is not
in there; PLAYBACK's 181 KB is where it went. Had 1000 not landed, that
line would read about 64 KB, so the Ogg probe turns out to be the one
path in this file big enough to be visible in the counters at all -- the
FLAC, WAV and MP4 probes are a few hundred bytes and would have proved
nothing.

**The pattern, which this file has recorded before and produced again.**
0902 called `usb_host_endpoint_halt()` on EP0 on the strength of the
call order being documented, without checking the argument was valid.
1000 named a window on the strength of the class being right, without
checking where the call sits relative to the report that closes it. Both
are the same move: reasoning about *what* the code does and asserting
something about *when* it runs. **A falsification condition is only
worth writing if it names a place the thing can actually be observed** --
and an untestable check is worse than none, because it reads as
verification and reports nothing.

### And the dead-field entry was wrong in both directions (1001)

The open list said `framewalk_t` still carries `frames` and `has_levels`,
"which nothing fills any more". Field by field:

| Field | Listed as | Actually |
| --- | --- | --- |
| `frames` | dead | dead -- never written, never read |
| `rate` | not listed | **dead** -- never written, never read |
| `has_levels` | dead | **live** -- written at two sites, read by `waveform.c` |

So the entry named a live field, missed a dead one, and was right about
exactly one of the three. `frames` and `rate` are removed. `has_levels`
stays: both writers set it `true` unconditionally, so it is vestigial
rather than filled, but `waveform.c` still tests it and dropping it means
dropping that guard too. That is a behaviour change -- a NULL or
zero-column envelope would be one test nearer being drawn -- and it does
not belong in a patch whose subject is deleting fields nobody writes.

**A list that is read but not checked decays into a claim about the
past.** This is the third time in two patches: 1000 struck out a
`covertag.c` entry closed by 0906, and now the entry beside it turns out
to have been describing a struct that had moved underneath it. 0811 and
0812 both name this failure. Neither stopped it, because what they
recorded was the lesson and not a habit of re-reading the list against
the code.

### So the whole list was read against the code (1002)

Three patches in a row found an open entry that had been closed by the
work and never struck, so 1002 stops finding them one at a time and
reads every remaining entry against the source. Four were stale. The
rest were checked and are correct, which is worth recording as plainly
as the corrections: **an audit that only reports what it changed cannot
be told from one that stopped early.**

Struck, with what actually closed them:

| Entry | Closed by | Checked against |
| --- | --- | --- |
| MP3 sidecar index never re-harvests finer | 0711 | `decoder_index_extract()`'s `num_frames <= index_count` exception |
| ALAC logs `Not find default parser` at every open | 0907 | `esp_log_level_set(SDEC_TAG, ...)` around both ALAC opens |
| Only 16-bit output; a 24-bit FLAC will refuse to play | 0803 | `fold_24_to_16()`, reached when `bits_per_sample == 24` |
| Seek on non-MP3 needs a provably constant byte rate | 0808 | the bullet contradicted itself; see below |

Confirmed still true, and left alone:

- **AMR has neither a test file nor a walk.** The corpus is twenty-one
  files and none of them is `.amr`. Unchanged and for the stated reason.
- **One lease covers the SD card and the USB port together.**
  `storage_io.c` still holds a single `s_lock`, not one per
  `storage_id_t`.
- **Nothing is peak-limited.** No limiter in `main/`.
- **No resampling, UAC 2.0 untested, bus power is USB 2.0 only.**
- **The size-only handle in `do_art()`**, the 0100-0105 and 0200-series
  compile-test caveats, the marquee not being eased, and gapless.

**One entry was not stale but corrupted**, which is a failure this file
had no category for. The seek bullet read:

    Seek on non-MP3 works only where the byte rate is provably
    constant, per above: PCM WAV, CBR ADTS, fixed-mode AMR. FLAC, Ogg,
    Every format this player decodes is seekable as of 0808, ...

An 0808-era edit replaced the entry's body and left its opening sentence
standing, so a single bullet asserted a restriction and then denied it
four lines later, mid-sentence, with a dangling `FLAC, Ogg,`. Every
other stale entry in this file is wrong in a way that reads as fluent
and has to be checked against code to catch. This one contradicts itself
in plain sight and survived anyway -- **which says the list was not being
read at all, rather than being read and believed.** That is the stronger
version of what 0811 and 0812 recorded, and the reason this audit was a
patch rather than a note.

**Threaded as a parameter, not held in a file-scope variable,** because
those two callers are on different tasks and can both be inside
`covertag.c` at once -- the file says so itself, at the top of
`load_tags()`. A shared "current priority" would be read by whichever one
happened to look after the other one set it. Eleven static functions
gained the parameter and fifteen `read_at()` sites now pass it.

**`albumart.c` had the same constant, on the path that matters most.**
`covertag_extract_art()` delegates plain ID3 to `albumart_extract_at()`,
which is every MP3 in an ordinary library, and it read at
`STORAGE_IO_PREFETCH` too. Fixing only `covertag.c` would have left the
common case exactly as it was, which is the kind of half-fix this file
has recorded twice in the last week.

**Not flashed.** `covertag.c` compiles clean; `albumart.c` and
`player.c` were checked by prototype agreement and by eye, since the host
stubs do not reach the JPEG and stream-buffer headers those files pull
in. What to look for: the two warnings absent from a boot log, and
`open playback` holding a larger share of its window on the first track.

### The USB route says UAC (0905)

0901 drew the A receptacle end on and argued for it on two grounds: it
extends the vocabulary `draw_usb_c()` already established for the C port,
and it is honest about which socket carries audio. Both still true, and
it is still the wrong answer.

A rectangle with a bar in it has to be learned before it says anything.
What it says once learned is "the USB port" -- and that is *less* than
what is known at the moment it is drawn. The route is not a port. It is a
UAC device that enumerated, offered a format this player can clock, and
won the arbitration in `audio_out.c`. Three letters say that, to anyone
who would know what a USB audio dongle is, which is everyone who has
plugged one into this thing.

It also stops the corner carrying two silhouettes to tell apart. A
rounded stadium for C and a squared rectangle for A are distinguishable
side by side, which is not the same as being distinguishable at a glance
next to a battery.

Scale 2 is forced rather than chosen: three glyphs at ark12's 7 px
halfwidth advance is 42 px, so 20 px either side of centre, inside
`SPK_HALF` (26). Scale 3 would be 30 px either side -- outside the hit
box shared with the mute button and into the slider's padded box at 82.
Measured from the real drawing code rather than the arithmetic alone:
ink lands at -20..+17.

The centring subtracts one scale from `gfx_text_w()`, which counts the
gap column after the last glyph that nothing draws into. Without that the
run sits a pixel right of the speaker and headphones it alternates with,
and they swap in place often enough for that to read as a twitch.

That is three answers for one icon -- trident, receptacle, letters --
which is worth recording as a pattern rather than as three separate
decisions. At this size a picture has to be *recognised*, and the budget
for recognition is much smaller than the budget for legibility. The
speaker and headphones survive because their silhouettes are already
known. Nothing about USB is.

### The same 998 ms, a third time (0904)

0903 moved the descriptor request out of the client event callback and
into `client_task`, on the reasoning that the callback must not block
because it runs inside `usb_host_client_handle_events()`. The reasoning
was right and the fix did not follow it far enough. The log from the
0903 build:

    I (7252) tab5_uac: ... alt 1: 2 ch, 16-bit, 48000 44100 Hz
    I (8250) tab5_hid: remote on itf 3: ...

998 ms. The same constant, for the fourth build running.

**Why it did not help.** `client_task` is the task that calls
`usb_host_client_handle_events()`. Moving the wait from the callback into
the drain loop moved it from *inside* that call to *between* two of them,
and the loop is still not running while the wait runs. The completion is
delivered by that call and by nothing else, so the wait was still waiting
for something only it could cause.

The 0903 comment said the drain loop was "where the event loop is free to
dispatch". The event loop is that task. It is not free while it is here.

**The rule, stated so the next person does not have to derive it:** on
this client, a blocking wait for a transfer completion is only correct if
the wait itself runs `usb_host_client_handle_events()`. Not "outside the
callback" -- *running the loop*. `report_desc_scan()` now pumps it in
10 ms turns for up to a second, checking the semaphore with a zero
timeout between turns.

Being out of the callback is still worth keeping. The host API contract
says the callback must return promptly, and opening devices and claiming
interfaces from it was outside that contract with or without the
deadlock.

**And the replug wedge, which was a separate bug wearing the same
symptom.** `DEV_GONE` arrives on `client_task` and closed the device
immediately. The interface release happens on whichever `hid_poll_task`
owned it, when its transfer completes with `NO_DEVICE`. Nothing ordered
those, so the close routinely beat the release -- and closing a device
that still has an interface claimed leaves the host library holding one
it cannot finish tearing down. The address is never released and a
replug, or even a VBUS off/on, produces no enumeration at all:

    I (194120) tab5_panel: USB bus power off
    I (195119) tab5_panel: USB bus power on
    (nothing)

Each open device now carries a claim count and a gone flag. The close is
performed by whoever brings the count to zero after the flag is set --
`client_task` if the polling tasks have already finished, the last
polling task if they had not. Both paths do the close outside the
critical section, since it is a host call that may block.

This is the third patch in a row on this file and the second time the
diagnosis was right and the fix landed short of it. Both times the log
had already said so: the 998 ms constant was in the first log of the
series, unchanged through three builds, and it was read as "the device is
slow" twice before it was read as "this number is a timeout and nothing
else."

**Verified in ctrltest.c**, which gains the ordering case: a claim count
and a gone flag exercised from two tasks with randomised delays, 600
runs, asserting the close happens exactly once and never while a claim is
live. Confirmed to fail on the old shape -- closing immediately on
DEV_GONE trips `g_close_with_claim == 0`.

**Not flashed.** Two things to look for: the attach-to-classification gap
being something other than 998 ms, and a replug enumerating.

### The descriptor request had never once worked (0903)

0902 stopped the boot loop and the log from the fixed build showed two
things it had not fixed.

**Three red lines on every boot.**

    E USB HOST: Get EP handle error: ESP_ERR_INVALID_ARG   (x3)

That is 0902's `usb_host_endpoint_halt/_flush/_clear(dev, 0)` being
refused. The default control endpoint is not owned by a claimed
interface, so there is no endpoint handle for the library to resolve. The
docs describe those calls in the context of tearing down a claimed
interface's endpoints, which is the case that does not include EP0 -- I
checked that the call order was right and never checked that the argument
was valid.

**And the port wedged.** With the flush refused, the abandoned URB stayed
outstanding; `DEV_GONE` then closed the device with a transfer live
against it, the address was never released, and a VBUS off/on produced no
enumeration at all -- not a failed enumeration, silence, until a reboot.
0902 turned a boot loop into a wedge. The comment predicting "a leaked
550 bytes rather than a panic" was wrong about which direction was safe.

**The real bug, which 0902 had only papered over.** Every log of this,
across three builds, showed the same gap between the device attaching and
the remote being classified:

    2079 -> 3077 = 998 ms
    2078 -> 3076 = 998 ms
    2076 -> 3076 = 1000 ms

Always the timeout, to the millisecond, never anything else. A device
that genuinely stalls sometimes does not produce that. `report_desc_scan()`
was reached from `client_event_cb()`, and the docs are explicit:

    "This function is called from within usb_host_client_handle_events().
     Do not block and try to keep it short."

The completion of the control transfer is delivered *by*
`usb_host_client_handle_events()`. Waiting for it inside the callback is
waiting for a call that cannot happen until the wait ends. It deadlocked
against itself on every device, every time, and the timeout was the only
thing that ever ended it.

So the descriptor request has never once succeeded in this program, and
the "the headset remote stalls EP0 for exactly this request" comment --
which is where the whole line of reasoning in 0902 started -- was an
invention. The device was never asked in a way it could answer.

**The fix is the shape Espressif's own class_driver.c uses.** The
callback records the address in a small pending list and returns.
`client_task()` calls `usb_host_client_handle_events()` with a 100 ms
timeout, and then, with the event loop free to dispatch, drains the list:
open, inspect, claim. The blocking work is on the far side of the call
that delivers what it blocks on.

The timeout moved off `portMAX_DELAY` because the loop now has work to
perform on its own return, and a device arriving during an otherwise
quiet bus would sit in the list until some unrelated event woke it.

0902's abandoned-URB handoff stays. It stops being the path every device
takes and goes back to being what it was meant to be: the rare case where
a device really does not answer.

**What the button table survives on.** `hid_report()`'s bit mapping was
inferred when no descriptor was available, and then confirmed by pressing
each key and reading the hex out of the log. That is observation, not
inference from the descriptor, which is why it should still hold once the
descriptor actually arrives and possibly reclassifies the interface. It
is the thing to watch on the first flash of this.

**Verified in ctrltest.c**, which gained a fourth case: a single-threaded
event loop, a wait that runs inside the dispatch (never completes) and a
wait that runs after it returns (always completes). Modelled rather than
compiled from `hid.c`, same caveat as the rest of that file -- what is
checked is the ordering, and the ordering was the bug.

**Not flashed.** The thing to look for is the absence of a 998 ms gap.

### The boot loop was a control transfer that came back late (0902)

`assert failed: xQueueGenericSend queue.c:936 (pxQueue)`, three reboots
in a row, backtrace through `usb_host_client_handle_events` into
`client_task`. The report:

    #3 xQueueGenericSend at queue.c:936
    #4 usb_host_client_handle_events at usb_host.c:948
    #5 client_task at main/hid.c:718

`pxQueue` is NULL, so something gave a semaphore that no longer existed.

**What it was.** `report_desc_scan()` put its completion context on the
stack and waited a second for the URB:

    ctrl_ctx_t c = { .done = xSemaphoreCreateBinary() };
    t->context = &c;
    ... xSemaphoreTake(c.done, pdMS_TO_TICKS(1000)) ...
    vSemaphoreDelete(c.done);
    usb_host_transfer_free(t);

On the timeout path that deletes the semaphore, frees the transfer, and
returns -- **with the URB still in flight**. When it eventually completed
the host dispatched `ctrl_cb()` on the client task, which read
`t->context` (a stack frame that had gone) and gave a deleted semaphore.
Freeing an in-flight transfer is separately forbidden; the IDF docs are
explicit that a transfer must not be in flight when freed.

**The comment above the function had the fact and missed the
consequence:** "the headset remote stalls EP0 for exactly this request."
A stall is handled -- it completes, with a status. The case that was not
handled is the device that never answers at all.

**Why it looked intermittent.** It is a race with enumeration timing,
and the loser is the ordinary setup. Device already plugged in at boot:
enumeration at ~2 s lands on top of the SD mount, the playlist scan and
the first cover prefetch, EP0 does not answer within the second, boot
loop. Device plugged in later, on a quiet bus: answers, or fails in a way
that leaves the log line and no crash. The log confirms the mechanism
without ambiguity -- 998 ms between "USB audio output attached" and
"remote on itf 3", which is the timeout expiring, and the panic
immediately after when the abandoned URB landed.

`E (1940) HUB: Root port reset failed` in the third cycle is the loop
making the bus worse, not a separate fault.

**The fix: two owners, and whoever finishes last frees.** The context is
heap allocated and holds an `abandoned` flag set and read under a
critical section, because the timeout happens on the claiming task and
the completion arrives on the client task. The timeout path sets the
flag, halts, flushes and clears EP0 -- so the URB is retired now rather
than landing at an arbitrary later moment -- and returns without touching
anything the callback will read. `ctrl_cb()` checks the flag: abandoned
means it owns the wreckage and frees it, and the completion having
arrived is exactly the moment when freeing the transfer is legal.

The halt/flush/clear ordering is the one the IDF docs give, and the clear
matters on its own: a halted control endpoint fails every later request
on that device.

Failing directions were chosen deliberately. A failed *submit* never
enqueued anything, so no callback is coming and the caller still owns
everything -- distinguished from the timeout for that reason. If the
flush calls fail, the context stays alive and owned by the callback,
which leaks ~550 bytes instead of panicking.

**A second bug in the same file, found while reading it.** The client
event callback kept one `static usb_device_handle_t s_open` for every
device on the bus. This device routinely has two -- a stick and the
headset, which is in every log -- so the second `NEW_DEV` overwrote the
first handle and leaked it, and the next `DEV_GONE` closed whichever was
current rather than the one that left. Closing a device out from under a
live interface claim is the same class of fault as the first, reached
from the other side. Now a four-slot table, closing the handle
`msg->dev_gone.dev_hdl` names. Four because this client opens devices
only to inspect their interfaces, and a hub full of them is a case the
port cannot power anyway.

**Verified in `texttest/ctrltest.c`,** under ASan, against a fake host
whose completion delay is a knob: completion inside the timeout, long
after it, and 400 runs landing right on the deadline. It reproduces the
logic rather than compiling `hid.c` -- the real function is welded to the
USB host API -- which is a real weakness worth naming, since the two can
drift. Confirmed to catch the original: reverting to the stack context
gives `heap-use-after-free`, freed by the waiter, written by the
completion thread. That is the panic, on a host, with a stack trace.

**Not flashed.** The USB host underneath is a fake, so what is verified
is the ownership shape and not the driver's real timing. The device that
crashes is the one to try it on.

### The speaker icon was lying (0901)

The left margin of the volume row has drawn a speaker since the first
version. It is also the mute button, so it had to be *something* -- but
what it drew was a speaker regardless of where the sound was actually
going. With headphones in, which is most of the time, the panel was
drawing the one output that was deliberately silent.

So this is not a new indicator. `audio_out.c` already arbitrates three
outputs, already keeps the answer in `s_route`, and already logs it; the
icon just never asked. Three shapes now, chosen by route:

- **speaker** -- the existing cone, unchanged.
- **headphones** -- a band and two cups. The band is an arc drawn one
  column at a time off the circle equation, because `gfx` has no arc
  primitive and this needs no line one. The cups are deeper than the
  band is thick; without that the silhouette reads as a croquet hoop.
- **USB audio** -- ~~the A receptacle seen end on~~ the letters `UAC`.
  See 0905; the receptacle shipped first and was replaced.

**Why the A receptacle and not the trident.** *(Superseded by 0905, which
replaced the receptacle with the letters `UAC`. Kept because the reasoning
about the trident still holds and because the receptacle's defeat is the
useful part -- a good silhouette that was still the wrong answer.)* The
trident is the logo everybody reads as USB, and it was tried first. At 30 px a stem, two
branches, an arrowhead and three differently-shaped feet collapse into a
smudge -- it is drawn for print and wants more pixels than this margin
has. Rendered it, looked at it, threw it away. The receptacle also
extends a vocabulary this panel already has rather than inventing one:
`draw_usb_c()` draws the Type-C port as a rounded stadium for external
power, so a square rectangle for the A port is the same idea about the
other socket, and the two are unmistakable side by side. It is honest
about the hardware too -- audio really does come off the A port and power
really does come in on C.

**What did not change, deliberately.** The tap still means mute. The
route is not a user choice on this device -- `arbitrate()` decides it and
the comment above it argues why -- so there is nothing for a tap to cycle
through, and stealing a control people already rely on to add one would
be a bad trade. If an override is ever wanted it belongs in the settings
panel's AUDIO tab, next to a note about the case where it cannot be
honoured: a device that cannot take the format already falls back to
analog per track, so a pinned "USB" would sometimes be visibly ignored,
and that needs explaining in a place with room to explain it.

**Not in the battery corner**, which was the first instinct. That column
is full: the moon sits at `s_w - 64` with its padded hit box reaching
y436, the battery icon starts at y460, and its digits end 40 px off the
bottom of the bar. Anything inserted there either collides with a touch
target or hangs off the panel -- and the layout section above already
warns against putting something back into that gap.

**Checked rather than eyeballed.** The three icons were rendered from the
real drawing code against the real `gfx` primitives (`/tmp` harness, same
shim `texttest/` uses) and their ink measured: speaker -13..+11 x,
headphones -16..+16, USB -15..+14, all inside `SPK_HALF` (26), so the hit
box is unchanged and none of them reaches the slider's padded box at 82.
`ui_draw()` clears the whole bar to `C_BG` before drawing, so the
differing footprints cannot leave residue when the route changes
mid-track -- which it does, on unplug, since `arbitrate()` re-runs from
`audio_out_write()` when the generation moves.

One ordering trap caught on the way: `fill_rrect()` is defined down with
the battery icons and the headphone cups need it, so it is forward
declared. This file has shipped a build failure for exactly that before.

### The font is 12px now, and glyphs have their own widths (0900)

Two changes that had to happen together. The table gained fullwidth
glyphs, and the renderer stopped assuming every glyph was one cell wide.

**Why the pixel size moved at all.** ark10 covered Latin and stopped, and
the obvious fix -- add Cyrillic and CJK to `RANGES` and regenerate -- does
not work, for a reason that only shows up if you look at the source. Ark
ships glyphs in three cuts. `monospaced` is a fixed cell. `common` is the
same height but double width, which is how CJK is drawn. `proportional`
has per-glyph widths *and* per-glyph heights. `gen_ark10.py` silently
dropped anything that was not exactly 5x10, and said so only in a
diagnostic nobody reads:

    if (w, h) != (GLYPH_W, GLYPH_H):
        skipped_width.append((cp, w, h))
        continue

So adding CJK ranges to ark10 would have produced a clean build, a
correct-looking diff, and zero new glyphs. That filter is also why `©`,
`®`, `¼`, `½` and `¾` were missing from Latin-1 -- the same problem in
miniature, already shipping, already written off in a comment.

**Cyrillic is why the size moved and not just the width model.** At 10px
Ark has no monospaced Cyrillic at all; it exists only in `proportional`,
16 rows tall with real ascenders and descenders. Cropping that to 10 rows
was measured rather than guessed: ink spans rows 2..13, and the
best-placed 10-row window still clips 25 of 153 glyphs. Not padding --
actual letters. Downscaling is worse, because at 4-6 px stroke widths
there is no antialiasing to degrade into; a hand-drawn pixel font resists
resampling by construction.

Ark ships 10, 12 and 16. Counted from the archive rather than assumed:

| | 10px | 12px | 16px |
| --- | --- | --- | --- |
| CJK Unified (unique) | 1,076 | 18,299 | 97 |
| Cyrillic (monospaced) | 0 | 151 | 151 |

12px is simply where this font is finished. 16px regresses hard on CJK --
crowd-drawn fonts get contributor effort unevenly across sizes, and 16px
kanji are mostly undrawn. So: 6x12 halfwidth, 12x12 fullwidth.

**The scale numbers came down to compensate**, and the physical size is
roughly unchanged -- see "Sizes are set for 294 PPI". The win is fidelity,
not size: 3x3 blocks instead of 4x4, over a glyph with more detail in it.

**What changed in `gfx.c`.** Every function that computed a string width
as `count * GFX_GLYPH_W(scale)` now sums each glyph's own advance.
`glyph_for()` returns a `{bits, w}` pair, rows widened from `uint8_t` to
`uint16_t` to hold 12 columns, and there are two notdef boxes rather than
one because a narrow box inside a run of fullwidth glyphs misaligns
everything after it.

`gfx_draw_text_tail()` needed the most work. It used to find its starting
byte with `tail_at()`, walking the string twice by cell count -- which
cannot work when cells have different widths. It now decodes once into a
bounded ring of the last `TAIL_MAX_GLYPHS` (96) glyphs and walks backward
accumulating real advances. 96 because the narrowest advance in this UI is
12 px at `LABEL_SCALE` against a 720 px panel, so 60 cells is the ceiling;
the bound matters because `browser.c` passes paths out of 512-byte
buffers.

**Nothing outside `gfx.c` needed to change**, which was checked rather
than hoped: all four uses of `GFX_GLYPH_W` were already inside `gfx.c`,
and every external caller asks `gfx_text_w()` for a total instead of
striding.

**Two bugs the work found, both in things that already looked done.**
`cp_is_wide()` treats `FF00-FFEF` as fullwidth, but `RANGES` never
included that block -- so fullwidth parentheses, which Japanese taggers
use constantly, drew a double-wide notdef box in space the renderer had
correctly reserved. Found by rendering a sampler sheet and looking at it.
And the marquee measured at scale 4 while the draw moved to 3, which would
have left the bounce distance computed for a width the string no longer
had.

**`護` (U+8B77) and `郎` (U+90CE) are still boxes.** They have no glyph in Ark at any size.
That is a gap in the font, not the table, and there is nothing to
generate. Expect a few boxes in CJK text rather than none.

**Verification: `texttest/`.** Host-side, ASan + UBSan, compiling the real
`gfx.c` and the real generated table -- not a reimplementation, for the
reason `seektest/` exists. 2,265 checks: measured width agrees with drawn
extent, `max_w` is never overrun, the clip window holds across the whole
marquee sweep, the tail walk keeps the tail past the ring bound, and
degenerate input is a no-op. The corpus is real-shaped strings plus
malformed UTF-8, because a tag can contain anything.

A green suite proves nothing on its own, so four bugs were deliberately
introduced and each confirmed caught: halfwidth-only `gfx_text_w()` (67
failures), an off-by-one in the ring index (UBSan null deref), a removed
clip edge (468), and a dropped advance in the budget check (90). The first
run also failed for a fifth reason -- a *test* bug, where a hand-picked
budget was larger than the suffix it asserted on, so the code correctly
kept more than the test expected. Budgets are derived from the suffix now.

**This is not a flash.** `texttest/` covers layout arithmetic and nothing
else -- not the DSI panel, not PSRAM, not how any of it looks at 294 PPI.
Every previous entry in this file that trusted host testing past its reach
was wrong to.

`id3_text_to_utf8()` replaced `id3_text_to_ascii()`. All four ID3 text
encodings now convert properly rather than being flattened -- in
particular encoding 0 is Latin-1, not ASCII, so 0xE9 is widened to a two
byte 'é' rather than copied. FatFs also moved to
`CONFIG_FATFS_API_ENCODING_UTF_8`, without which filenames would have
stayed codepage 437 and the browser would have been the one place left
showing mojibake.

## Prefetch, and going back

`mediacache.c` holds three entries -- previous, current, next -- keyed by
path, each carrying the compressed cover and the `framewalk_t` envelope.
Roughly 350 KB of PSRAM in steady state. The **decoded** cover is
deliberately not cached: 700x700 RGB565 is a megabyte and the hardware
JPEG codec rebuilds it in single-digit milliseconds.

`media_task` prefetches the *next* track's cover once the playing
track's own cover and envelope are in hand, gated on how full the PCM
ring is -- start above 75%, drop the result if it fell below 50% while
reading. Two thresholds rather than one because a single one chatters at
the boundary. The gate exists because prefetch is a second reader on the
device the decoder is already reading, which on a USB drive is exactly
the contention the one-task-for-both design was built to avoid.

**The ring is `PCM_RING_BYTES`, 3520 KB, about 20 s at 44.1/16/2, in
PSRAM and created static.** That is the number every log shows --
`dropped 3519 KB of queued audio` at a skip, `tail: 3519 KB of this
track still to play`.

*(The rest of this paragraph described the ring as 64 KB and then as
256 KB, and other sections had it at 10 MB. All three were true once and
none is now; corrected in 1008. The history is kept because the reverted
attempt is the reason the current one is shaped the way it is.)*

Patch 06 moved it to 256 KB in PSRAM via `xStreamBufferCreateWithCaps()`
and the heap then corrupted -- `tlsf_free: block already marked as
free`, reproducibly, when skipping tracks fast enough that some decoded
no blocks at all. Patch 11 reverted it to 64 KB and the same sequence
ran clean. The mechanism was never established.

**The ring got its size back later without reinstating that call**, which
is the part worth understanding: the storage is allocated once in
`app_main()` and never freed, and `xStreamBufferCreateStatic()` is built
on top of it. That allocates nothing, so there is no allocator to pair a
free with, and `vStreamBufferDelete()` on a static buffer frees nothing
-- it checks the statically-allocated flag and returns. The per-track
delete therefore stays exactly as it was, with all its reasoning about
handle ordering intact, and nothing on the per-track path allocates or
frees at all. The authoritative version of this argument is the comment
above `PCM_RING_BYTES` in `player.c`.

The gate reads a **published integer**, not the ring handle.
`media_task` calling `xStreamBufferBytesAvailable(s_pcm)` directly is a
use-after-free -- the decode loop frees the ring between tracks, and an
`if (!s_pcm)` guard does not help because the pointer can be loaded
before the check and used after the free. It crashed on hardware as a
TLSF assert on an unrelated later `free()`, which is what a stray read
into freed heap metadata looks like.

`media_task` is already the lowest priority task in the program -- 1,
against 4 for the UI and 6 for the I2S writer -- but **priority decides
who gets the CPU, not who gets the device.** A background task at
priority 1 issuing a 512 KB read still puts that read in the same queue
the decoder is waiting on. So the throttle is time and depth: the cover
waits 700 ms after a track change and the envelope 2.5 s, and both then
wait for the ring to reach 60% before touching the card. The wait is
bounded at 8 s and says so in the log if it expires, because a late
cover beats no cover and a gate that never opens should not be silent.

Only the cover is prefetched. The frame walk reads the whole file, which
on a 60 MB FLAC is a background reader doing more I/O than the decoder
for the entire track, and a late envelope is invisible -- the bar
degrades to a plain slider and then becomes a waveform, which is what it
already does.

Shuffle does not prefetch. `playlist_peek_next()` returns NULL for it on
purpose: the choice is made by `esp_random()` when asked, so predicting
it would mean fixing it a song early and making the played-bitmap lie if
the track is skipped.

**Double-tapping previous** walks the play history rather than the list.
`playlist_prev()` is index-1, which is right in list order and wrong
under shuffle -- `playlist.h` said as much already. The stack is eight
deep and pushed when a track actually starts, not when one is requested.

The subtle part is the anchor. The two taps are up to 400 ms apart and
the first one already requested a track change, so by the time the
second arrives that track may or may not have started and pushed itself.
Popping the top would mean "the track before the one I was on" or "the
track I was just on" depending on how fast the card is -- a back button
that sometimes goes forward. So the second tap is resolved against the
track that was playing when the *first* tap landed.

**Next greys out at the end of a folder.** `playlist_has_next()` is
deliberately not `playlist_peek_next() != NULL`: under shuffle there is
no *predictable* next track but there certainly is one, so peek returns
NULL and this returns true. Greying under shuffle would claim the
playlist had ended when it had not. Repeat-one maps to list order,
matching what the skip button actually does -- repeat-one governs what
happens when a track ends, not what skip means. Prev is never greyed,
because it always does something.

**The seek row is the same shape before and after the scan.** With no
envelope yet, the row draws as a flat full-height block split at the
playhead rather than as a thin groove with a thumb. The thumb was
misleading -- it says "grab me", when the whole 72 px block is the
target both before and after -- and swapping a 6 px groove for a 72 px
waveform mid-glance read as the control being replaced rather than as
detail arriving. Two fills, not a per-column loop; at 720 px that is 720
`gfx_fill_rect()` calls saved per repaint for as long as the scan takes.

**Covers are fitted to the box in both directions.** Previously a cover
smaller than the panel was centred at native size, so a 300 px cover
used a sixth of the area it was given and looked like a thumbnail that
had failed to load. The JPEG path needed no new arithmetic -- the 16.16
step is simply below 1.0 when enlarging. The PNG path did: pngle streams
source runs and never offers a bitmap to sample, so the mapping has to
run forwards, from source pixel to destination edges. Expressed as edges
(`i*cw/iw` to `(i+1)*cw/iw`) rather than as a step, because computing
both with the same expression is what makes adjacent runs abut instead
of leaving seams. Nearest neighbour either way, so enlargement is
blocky; that is honest, and a bilinear pass would trade blocky for soft
at four reads per output pixel during playback.

**Every button logs.** Transport presses are logged once where they are
dispatched rather than per case, so a new action cannot be added and
forget to log itself; `ui_action_name()` sits next to the enum for the
same reason. Volume is excluded because a drag emits one every poll --
fifty a second would bury everything else -- and its release is logged
by its own case. Browser presses log before the switch acts, so a press
that turns out to do nothing (page up at the top of a list) still shows
as received: a button that is working and a button that is not both look
like silence otherwise.

**Touch and screen transitions.** `ui_task` samples the panel once per
iteration and passes that sample to whichever screen is up. An iteration
that changes screens therefore must not also dispatch input, or the press
that opened the chooser arrives as the chooser's first tap -- at the
folder icon's coordinates, which is list row 10. Gating the touch source
does not help here, because the value has already been read. The opening
branch draws and `continue`s; the closing branch always did.

**`sdkconfig.defaults` is only read when `sdkconfig` does not exist.** An
existing build directory keeps the old value, so the font renders
accents and the filenames still do not -- which looks like a font bug and
is not. The tell is a single replacement character where one accent
should be: `Bôa` is four bytes in UTF-8 and three in codepage 437, and a
lone 0x93 is invalid UTF-8, so it collapses to exactly one U+FFFD.
Two would mean something else entirely. After pulling this change:

    rm sdkconfig && idf.py reconfigure

or `idf.py fullclean`. Verify with `idf.py menuconfig` under
*Component config -> FAT Filesystem support -> API character encoding*.

This is not a one-off. It applies to **every** symbol added to
`sdkconfig.defaults` after a tree's first build, and the failures are
all of this shape: the file states a decision, the comments above it
explain the decision, and the build ignores both. The optimisation level
is the one that bites hardest -- `CONFIG_COMPILER_OPTIMIZATION_PERF`
(`-O2`) sitting inert while the binary is built at `-Og`, so the decode
loop is several times slower than every timing constant in `player.c`
assumes. The top-level `CMakeLists.txt` warns about that particular one
at configure time, because a stutter caused by an optimisation level
looks exactly like a stutter caused by a slow card.

`-Og` is still a legitimate thing to ask for. Stepping through
`play_file()` at `-O2` is hopeless. The warning says so.

Two things change when the switch does take effect:

* **More warnings.** GCC's dataflow analysis is far stronger at `-O2`,
  and code that only ever compiled clean at `-Og` has not really been
  checked. `0608` is the worked example: `scroll_geom()` returns without
  touching its out-params when the list fits, and nothing proved the
  separate `s_count > rows` test agreed with it.
* **The lock-free handoffs get their first real test.** `s_wave_ready`,
  `s_fade_out`, `s_pcm_flush` and `s_rg_pending_ready` are all
  write-the-payload-then-set-the-flag pairs across tasks. `volatile`
  stops the compiler caching or reordering *those* accesses; it is not a
  barrier and does not order a non-volatile payload write against the
  volatile flag write. At `-Og` that is academic because nothing moves.
  If something starts misbehaving after switching, look here first, and
  reach for release/acquire rather than for more `volatile`.

### Cover art and tags beyond MP3

`covertag.c` dispatches on magic bytes and reads whichever container is
in front of it. `albumart.c` keeps the ID3v2 reader -- it is bound up
with the APIC layout and the v2.3/v2.4 size trap, and moving it would
have been churn -- but grew `_at()` variants so the same parser can be
pointed at a tag that is not at offset 0.

| Container | Picture | Tags |
| --- | --- | --- |
| MP3 | APIC frame | TIT2 / TPE1 / TALB |
| FLAC | PICTURE block (type 6) | VORBIS_COMMENT (type 4) |
| M4A / MP4 | `moov.udta.meta.ilst.covr` | `(c)nam` / `(c)ART` / `(c)alb` |
| Ogg Vorbis, Opus | base64 `METADATA_BLOCK_PICTURE` | VorbisComment |
| WAV | ID3v2 in an `id3 ` chunk | same |

Notes on the parts that bite:

- **`meta` carries four bytes of version and flags before its children**
  and nothing else on the MP4 path does. Walking it like a plain
  container puts you four bytes out and every child type reads as
  garbage.
- **`"\xA9ART"` does not mean what it looks like.** C reads `\xA9A` as
  one hex escape, because `A` is a hex digit, so the literal is three
  bytes and no M4A ever reports an artist. Same for `alb`. Only `nam`
  is safe, which is the worst outcome -- it would have looked fine.
  Written as `"\xA9" "ART"`.
- **Ogg needs real page reassembly.** A cover spans pages via 255-byte
  segments, so it cannot be read from a fixed prefix the way the three
  strings could. The packet buffer grows geometrically; growing it by
  each 255-byte segment is four thousand reallocs for a 1 MB cover, on
  a heap shared with a running decoder.
- **base64 is decoded in place**, because the alternative is holding the
  encoded and decoded copies of a megabyte at once.
- **FLAC files can hold several pictures.** Type 3 (front cover) wins;
  anything else is kept only as a fallback, so a file with a liner-notes
  scan first still shows the sleeve.
- **A leading ID3v2 tag on a FLAC or Ogg is skipped.** Not legal in
  either, and taggers do it anyway.
- Sizes read from the file are capped at `COVERTAG_MAX_IMAGE` (4 MB)
  before any allocation, since a corrupt length field is otherwise a
  `malloc()` of whatever the corruption says.

`ESP_ERR_NOT_SUPPORTED` from the dispatcher now means "no parser for
this container" and `ESP_ERR_NOT_FOUND` means "no picture in the file" --
which is what `do_art()`'s log line used to claim while actually meaning
"no APIC frame".

Five Latin-1 characters are still missing -- © ® ¼ ½ ¾ -- because Ark
draws those fullwidth and this table is halfwidth-only. They render as a
notdef box, as does anything past Latin Extended-A. A box rather than
`?`, because `?` reads as a character the file actually contained.

No title in the tag falls back to the filename.

### Time display

Row 3: elapsed at the left edge, remaining at the right, both drawn as
seven segments. No font is linked and vendoring one for two timestamps is
not worth it -- seven segments cover 0-9 and a colon, which is all of
MM:SS, and the minus sign for the remaining time is segment `g` drawn on
its own, which is what keeps it aligned with the digits beside it.

Duration comes from `decoder_duration_sec()`, which minimp3 answers
directly and everything else answers through the container probe:
`MP3D_SEEK_TO_SAMPLE` builds the index up front so `ex.samples` is known.
The esp_audio_codec simple decoder exposes `frame_size`, not stream
length, so FLAC and WAV report 0 -- with nothing to subtract from, the
right-hand clock reads `00:00` in the dim colour and the bar stays a plain
groove rather than inventing a scale.

### Duration comes from the container when the decoder cannot say

`decoder_duration_sec()` was minimp3-only, so FLAC, WAV and Ogg reported
0 and the bar stayed empty. That is an API ceiling rather than a missing
feature: esp_audio_codec's simple decoder exposes `frame_size`, not stream
length, and its parsers are forward-only over a stream.

But the decoder is not the only thing that knows. Every one of these
formats states its own length in a fixed place, readable with two or three
`fread()`s and no audio decoded at all. `duration.c` is the backup, called
only when the backend returns nothing, and cached because the UI asks once
per track and the answer cannot change.

| Format | Where | Note |
| --- | --- | --- |
| FLAC | STREAMINFO, always the first metadata block | 36-bit sample count, packed across byte boundaries |
| WAV | `data` size / `fmt ` byte rate | chunks are walked, not assumed adjacent |
| Ogg | granule position of the last page | found by scanning back from EOF |
| MP4 | `mvhd` duration / timescale | v0 and v1 differ by 12 bytes |

Four things here are the difference between right and plausible:

- **Opus granule is always in 48 kHz units** regardless of the stream's
  own rate. Dividing by the sample rate in the header is the classic way
  to get a duration wrong by a constant factor, and `OpusHead` even
  carries an input-rate field that invites exactly that. The codec is
  identified from the first page and the divisor chosen from that.
- **WAV chunks are walked.** Anything that writes LIST/INFO metadata puts
  it between `fmt ` and `data`, so a probe that assumed `data` at offset
  36 reads the metadata length as the audio length.
- **The Ogg window is 64 KB** because the spec caps a page at about that,
  so the last page's start is always inside it -- one sequential read
  rather than a walk of the file. It is allocated in PSRAM and freed, not
  held as a static: 64 KB of internal RAM for one question per track, on a
  chip with 384 KB of it and a USB host stack next door, is not a trade
  worth making.
- **Not file size / bitrate.** Right for CBR, drifts badly on VBR, and a
  seek bar that lies is worse than one that stays empty -- which is the
  call the code already made.

The format is sniffed from the magic bytes rather than taken from the
extension. The caller already chose a decoder by extension; a probe that
trusted the same wrong extension would return a confident number for a
mislabelled file instead of nothing.

This does not make those formats seekable, and by 0705 all of them are
anyway, by three different routes: a proven-constant byte rate for WAV,
CBR ADTS and AMR, a frame-header bisection for FLAC, and a page-granule
bisection for Ogg -- which is exactly the scan described here, applied
as a search rather than as a single read.

### The frame walk, and why it is gone (historical, 0206)

`framewalk.c` used to answer two questions in one sequential pass over a
file: how many frames a format with no stated length has, and what the
seek bar should look like. It is deleted. The reasoning is kept because
what it got wrong is the useful part.

The duration half was sound. Raw ADTS, AMR and a CBR MP3 with no Xing
header state nothing about their length, so counting frames is the only
honest answer, and since every one of those formats puts its own length
in its header the walk was header-skip-header with the audio never
touched -- I/O bound rather than CPU bound.

The waveform half was not. It drew `global_gain`, which sits in MP3 side
info at a fixed bit offset and is therefore free to read, and which is
**the encoder's quantisation-step choice** -- how many bits a granule was
worth. That correlates with loudness, because a busy passage gets more
bits, and it is not loudness: two granules with the same `global_gain`
can sound nothing alike. It was a whole-file read producing a proxy, and
calling it ReplayGain (which the first patches did) was wrong.

The lesson worth keeping is narrower than "it was a proxy". It is that a
number which is cheap, correlated, and shaped like the thing you want is
the hardest kind of wrong to notice -- the envelopes it drew looked
plausible for months. The tell was in the data once there was something
to compare against: `global_gain` envelopes spanned levels 132..210,
because the value never approaches zero even in silence, where the real
amplitude envelopes that replaced them span 1..182. A narrow band that
never reaches the floor is what a bit budget looks like.

What replaced it is in **"ReplayGain, and where the waveform comes from"**
below. `framewalk.h` keeps only `framewalk_t`, which the cache, the
sidecar and `waveform.c` still pass around; renaming it would touch far
more than it would explain.

Two details from the walk are worth carrying forward because they are
about file formats rather than about the walk, and anything that parses
these containers will meet them again:

- **Arbitrary bytes adjacent to audio are a category, not a list of
  bugs.** A 130 KB ID3v2 tag full of PNG is full of bytes that look like
  a sync word; so is an ID3v1 or APE tag at EOF. Resyncing a byte at a
  time through either locks onto noise and parses nonsense. Both are
  skipped by their length fields. The ID3v2 size field is syncsafe --
  seven bits per byte, high bit always clear -- so the length can never
  itself contain a false sync.
- **A granule with no main data is silence, and encoders leave junk in
  the gain field.** `part2_3_length == 0` means no scalefactors and no
  Huffman data, and `global_gain` is then whatever was left there: LAME
  writes 210. Read literally, every LAME MP3 opened and closed at four
  fifths of full scale. Those are real frames correctly parsed -- the
  encoder delay at the head and the flush padding at the tail, 51 of them
  on a 2.6 minute track.

## Ogg loudness without decoding anything (historical, 0206)

Vorbis and Opus have no equivalent of MP3's `global_gain`, so the frame
walk approximated their envelope from **page sizes**: both codecs are
always VBR, a VBR encoder spends bits where there is something to encode,
and a silent passage is a handful of bytes where a loud one is hundreds.

That was a proxy for a proxy, and it is gone with the walk for the same
reason. It is recorded here because the reasoning was explicitly labelled
as an approximation in the code and in this file, and it still ended up
drawn on screen as though it were a waveform for as long as the walk
existed. Labelling a number honestly in a comment does not stop a UI from
presenting it as the thing it approximates.

Ogg now gets its envelope the same way every other format does: off the
decoded PCM of a normal play. The codec was always going to run; the
envelope is a by-product of it running.

### The envelope is the seek bar

`waveform.c` draws the walk's output as a single shape at the bottom of
the screen: an **upper-sideband envelope standing on the seek bar's
baseline**, spanning the bar's width, with everything played drawn in red
and everything still to come in grey.

It used to be a band framing the cover art, mirrored about a centre line,
drawn once when the scan landed and then left alone. Three things changed
and they are one idea.

- **Upper sideband only.** The mirrored envelope spent half its pixels
  restating the other half. `global_gain` is a magnitude -- there is no
  sign to it -- so the lower lobe carried nothing the upper one did not.
  Dropping it buys the same detail in half the height, which is what makes
  it fit in the transport bar at all.
- **Moved down, into the bar.** Up in the artwork area it competed with
  the cover for the same rectangle, which is why it needed a cutout, why
  the cutout had to be a guess at the cover's size, and why a cover larger
  than 548 px got overdrawn at the edges. None of that exists now: the
  artwork area is the artwork's.
- **Combined with the seek bar.** There were two horizontal, left-to-
  right, time-axis objects on screen, one showing the shape of the song
  and one showing the point reached in it. Same axis, drawn twice. The
  slider's groove, fill and thumb are gone; the envelope's own columns
  carry the position as a colour boundary, with a 3 px white playhead at
  the split because across a quiet passage the boundary is only 5 px tall.

Consequences of the merge:

- **It is redrawn on every repaint, not once.** It has to be -- it changes
  colour as the track plays. That in turn means it needs its own copy of
  the levels: `waveform_set()` takes one, because the scan task's
  `framewalk_t` is overwritten the moment the next track starts scanning
  and the UI is now a live reader of it. It is a kilobyte.
- **The scan still runs alongside playback**, on the lowest priority task,
  with its own `FILE*`, and a new track still aborts the old walk. Nothing
  about the scan changed; it just hands off differently.
- **The bar falls back to a plain slider while there is no envelope** --
  during the scan, and permanently for a format with no per-frame
  loudness. A track has to stay seekable in the meantime, and a control
  that vanishes for the first few seconds of every song is worse than one
  that changes appearance once.
- **The hit target is the drawn shape**, the full 64 px, not a padded band
  around a line. Pressing a tall column and having nothing happen reads as
  the bar having gone dead.
- **The bar grew and the artwork lost the rows.** The artwork gave up a
  strip it was not using and got back the whole of its middle, which the
  envelope used to draw a frame around. It has since given up the rest of
  what it was not using -- the cover is a 720 px square now and the bar is
  everything below it.
- **The envelope is still scaled from the track's own minimum**, not from
  zero. `global_gain` on a quiet passage is a low number rather than 0, so
  a straight 0-255 mapping draws every track as a slab with no shape in
  it. Silent columns are excluded from that range -- see below -- or a
  track that opens with encoder padding would put the floor at 0 and
  reintroduce the slab through the front door.
- **There is still a floor on the drawn height**, 5 px rather than 6. The
  quiet end of a normalised envelope is a one-pixel hairline, invisible at
  arm's length on a 294 PPI panel, and a hairline in the middle of a
  slider reads as the slider being broken rather than as the song being
  quiet.

The walk still supplies the duration when nothing else could, so on a
Xing-less MP3 the bar goes from empty to filled partway through the song
rather than staying empty for all of it.

### Length and seekability are different questions

The first drag guard tested `len_sec`, on the reasoning that a bar with no
scale has nothing to drag against. That was right until `duration.c`
landed, and then it was wrong: an Ogg reads its length out of the
container and has a full, correct, moving seek bar that **nothing can seek
within**. The drag went through, the thumb followed the finger, and the
player logged `seek ignored: this backend cannot seek` on release.

So `decoder_can_seek()` is asked separately, and the bar has three states
rather than two:

| State | Drawn as | Drag |
| --- | --- | --- |
| Seekable | full slider with a thumb | yes |
| Length known, not seekable | groove with progress filled, no thumb | no |
| No length | bare groove | no |

The middle row is the one worth the extra case. The position is real and
worth showing; the missing thumb is what says not to try dragging it.

### The walk declines formats it cannot parse (historical, 0206)

`framewalk_supports()` read four bytes before the scan task committed to
anything, because without it an Ogg was read end to end -- a whole file
off the card, in contention with the decoder reading the same card for
the same track -- to produce zero frames and a log line saying so.

Gone with the walk. The general form is worth keeping: **a cheap check
that a long operation is worth starting belongs before the operation, not
inside it.** Nothing now needs it here, because nothing schedules a pass
over a file at all.

### The seek target reads as a time

A seek drag reading "46" would be a unitless number on a bar whose two
ends are already clocks. That observation is what eventually retired the
bubble entirely: if the answer wants to be MM:SS, the two MM:SS fields
already on screen are the place for it. A format with no duration has no
target to show and the drag is refused anyway -- see "Length and
seekability are different questions".

### The finger bubble is gone

It was a 128 px disc raised above the finger during a drag, showing MM:SS
for seek and a percentage for volume. The argument was that the thing
being adjusted should not sit under the hand.

Row 3 answers that better and already existed. What a seek drag adjusts
is a position; row 3 shows positions, in 20x38 seven-segment digits at
the two ends of the panel, where the eye already goes for that number.
The bubble was a second, smaller rendering of the same value somewhere
worse. Volume needed no readout at all.

What it cost was out of proportion:

- **It was the only thing `ui_task` ever drew above `s_bar_top`.** It
  reached onto the cover art, which is not cleared each frame, so erasing
  it needed a saved strip -- `s_bubble_bg`, captured by
  `ui_capture_background()` from five call sites across three tasks,
  freed and reallocated by `media_task` while `ui_task` memcpy'd 190 KB
  out of it, with `s_bubble_top` and `s_bubble_h` torn alongside. A
  shared pointer with no owner, which is the one thing this file has a
  rule against. See "Never share a handle across tasks".
- **It cost a second `gfx_blit()` per drag poll**, plus two 190 KB
  memcpys, at 50 Hz. Roughly 40 MB/s of PSRAM bandwidth on a bus the DPI
  peripheral reads flat out and cannot be made to wait for.
- **It made "the two writers own disjoint bands" false.** It is true now:
  `media_task` owns rows 0..`UI_ART_H`-1, `ui_task` owns the rest, and
  the only thing they contend for is the transfer.

`BUBBLE_ABOVE` had to exceed `SEEK_Y` or the bubble overlapped the bar,
and that constraint is listed above as load-bearing. It is gone with the
thing it constrained. Row 7's hit-box spacing is still real. Do not
reintroduce the first by putting something else above the bar.

`gfx_ring()`, `gfx_ring_arc()` and `gfx_draw_small_time_centred()` went
with it; nothing else called them.

### Seek

`decoder_seek_sec()` is MP3-only. `MP3D_SEEK_TO_SAMPLE` built a
sample-accurate index at open time, so `mp3dec_ex_seek()` lands exactly
rather than guessing a byte offset. The esp_audio_codec simple decoder has
no seek entry point -- its parsers are forward-only over a stream -- so
FLAC and WAV return `ESP_ERR_NOT_SUPPORTED` and the drag is ignored with a
log line rather than treated as a failure.

Two things happen on a successful seek that are easy to leave out:

- **The queued audio is dropped**, or ~0.37 s of the old position plays
  out after the jump and sounds like the seek was ignored and then took
  effect late. Dropped by asking the writer -- `s_pcm_flush` -- and
  **not** by calling `xStreamBufferReset()`, which was the cyan flash.
  See "The cyan flash was xStreamBufferReset()".
- **`frames_out` is re-anchored.** It is the source of the elapsed clock,
  so without this the time counts on from where it was instead of from the
  new point.

`mp3dec_ex_seek()` counts in int16 values across all channels, the same
units as `ex.samples`. Seeking to `sec * hz` lands at half the intended
point on stereo.

### Seeking without seek data: prove the line, then draw it

`cbrseek.c` makes the esp_audio_codec backend seekable for the formats
whose time-to-offset mapping is a straight line. PCM WAV, constant
-bitrate ADTS AAC, and AMR at a fixed mode.

**Nothing about the decoder changed; the file moved.** The simple
decoder has no seek entry point and is not going to get one -- it takes
bytes and returns PCM. But a forward-only parser does not care where the
bytes came from once it has read the container header, so the jump is an
`fseek()` plus a reset of `in_len`/`in_pos`/`eof`, and the parser is
never told. That is `esp_codec_seek()`, and it is fifteen lines.

**The proof is the whole feature.** `duration.c` refuses file size over
bitrate in those words, and it is right to -- correct for CBR, badly
wrong for VBR, and a seek bar that lies is worse than one that does not
move. The difference here is that the linearity is checked:

| Format | How it is established |
| --- | --- |
| WAV | `fmt ` states the byte rate and `data` the extent. PCM is linear by construction; the format tag is checked so a compressed WAV is refused rather than mapped |
| ADTS | short runs of frames are walked at five points across the file and the mean frame length has to agree between them within 1.5%. `buffer_fullness == 0x7FF` is the stream declaring itself VBR and is refused before any of that |
| AMR | frame size is a function of the mode bits, so a constant mode is a constant size. Checked by reading one byte where each header must be |

A file that fails is exactly where it was: no length from here, no seek,
the bar stays a groove. **The failure mode is losing a feature, not
gaining a wrong answer**, which is the same trade the `global_gain`
envelope got wrong and took months to notice.

Details that are load-bearing rather than tidy:

- **The ADTS sample points must be spread, not consecutive.** A VBR
  stream's mean frame length differs between a quiet passage and a loud
  one by far more than the tolerance; five windows in the same place
  would agree with each other and prove nothing.
- **The AMR check includes the end of the file, and that is not one of
  the five points.** The frame count is the file size divided by the
  size the *first* frame declares, so a file that switches to a smaller
  mode part way has more frames than that arithmetic says and every
  evenly-spaced sample point lands before the switch. The tail is the
  one place the miscount shows.
- **One valid-looking ADTS header is not a frame start.** `FF F1`
  appears inside AAC payload, so a candidate is accepted only when the
  length it declares lands on another valid header, three deep. Same
  trap that ID3 tags full of PNG set for MP3 sync scanning, same answer.
- **A seek before the first decoded frame is refused.** The parser has
  not read the header yet, and moving the file would take it away from
  it. `produced` is that flag; the window is a few tens of milliseconds
  and `ESP_ERR_INVALID_STATE` is the honest answer inside it. Do not
  "fix" this by reopening the decoder handle -- that puts the WAV parser
  back at *expecting a RIFF header* and hands it PCM.
- **`storage_io` class is PLAYBACK**, not `duration.c`'s PREFETCH. Every
  caller is `decoder_open()` on the decode loop, which is the pause
  before the first sample. The PREFETCH classification elsewhere is a
  wart this file declined to copy.

**It also gives raw ADTS and AMR their duration back**, which is the one
thing 0206 traded away when the frame walk was deleted: those formats
state nothing about their own length and had no source for it but a
whole-file count, so they read `--:--` until a full play recorded one in
the sidecar. A byte rate that has been *proven* constant over a known
extent is that count without the read. It is asked second, after
`duration_probe()`, because a stated length is a fact and this is a
derivation, and the two only disagree when the derivation is wrong.

Accuracy: WAV and AMR land exactly, both having frames at a pitch known
to the byte. ADTS lands on the first frame boundary at or after the
target, so the error is under one frame (23 ms) plus whatever the
tolerated 1.5% has accumulated -- under a second at the far end of a
ten-minute track. A finger on a 720 px bar is asking for about 800 ms of
that track, so the mapping is finer than the request.

Host-tested under ASan and UBSan: synthetic WAV (with and without a
LIST chunk between `fmt ` and `data`), CBR and VBR ADTS with and without
a leading ID3v2 tag, fixed-mode and mode-changing AMR, and a file of
noise; then 400 mutated cases per format (truncation, byte flips,
`0xFF` injection) with a seek attempted at every second of the claimed
duration. Not IDF-built, which is the caveat the 0200 series already
paid for twice.

#### The parser has to be reopened, not merely moved (0701)

0700 moved the file and left the decoder handle alone, reasoning that a
parser which has already read the container header does not care where
the following bytes come from. That is true of ADTS and it is not true
of WAV: esp_audio_codec's WAV decoder tracks its position within the
`data` chunk it was told about, so a jump landed it somewhere it did not
expect and the next `process()` call failed.

**A decode error is how a track ends.** That is what made a wrong
assumption into an audible fault rather than a glitch -- the symptom was
not a click at the seek, it was the track playing over itself. See the
next section.

So the handle is closed and reopened on every seek, and fed a preamble
that describes the audio at the new offset: a synthesised 44-byte
RIFF/WAVE header for WAV, the file's own magic for AMR, nothing for
ADTS. `cbr_resume_preamble()` builds it.

- **The synthesised header is not the file's own.** The file's `data`
  length describes the whole track and the parser is being handed the
  middle of it, so the length written is what is left from the seek
  offset. A parser that clamps its output to the declared length then
  stops at the real end of the audio rather than a track's worth of
  bytes past it.
- **It re-declares the stream as plain PCM**, which is why `probe_wav()`
  now reads the extensible subformat GUID rather than accepting tag
  0xFFFE on sight, and why IEEE float is refused. Not because float is
  non-linear -- it is perfectly linear -- but because describing it as
  PCM at every seek would be a lie the parser believes.
- **A failed reopen leaves no handle**, so `esp_codec_read()` checks for
  one and reports the stream unusable. Ending the track honestly is the
  only available answer; calling into a NULL handle is not.
- **The `produced` guard is gone with the assumption it protected.**
  0700 refused a seek before the first decoded frame because the parser
  had not read the header yet. With the header now supplied on every
  seek, there is no such state.

The general form, which is the part worth keeping: **a parser handed
bytes from a new position is in an undefined state unless something
defines it.** Reopening plus a preamble is cheap -- one alloc and one
free on a path already dropping seconds of queued audio -- and it buys
the property that every seek starts the parser exactly where a fresh
open would.

#### The rate-change drain asked the wrong question (0722)

Reported as Ogg jumping to the end of the track at a mixed-rate
boundary. The log says it plainly once you know to look:

```
tail: 3516 KB of this track still to play; holding the screen
...
the finished track has played out    <- 111 ms later
```

**Twenty seconds of audio, discarded.** No `draining first` line
anywhere near it.

The drain was conditional on `s_ring_play != s_ring_fill`. The ring
switch happens BELOW that test, after the reconfigure -- so at an
ordinary boundary the previous track's ring is still both `play` and
`fill`, the test is false, and `audio_out_set_format()` reconfigures
the I2S clock with the outgoing track still queued in the ring the
writer is reading. Reconfiguring disables the channel; that audio never
sounds.

It has presumably been wrong since the drain was written, and worked
whenever the indices happened to differ -- which is when the PREVIOUS
boundary left them that way. That is why it appeared on some
transitions and not others, and why 0710 and 0712 both found the drain
behaving as documented when they looked at it.

**The right question is whether anything is still queued for the
writer**, which is one thing and directly observable:
`xStreamBufferBytesAvailable(s_ring[s_ring_play]) > 0`. Same test for
entering the drain and for staying in it.

Two things fall out of the fix. 0720's dip now has something to ramp --
it was arming against a ring that was about to be thrown away, which is
why the board reported it starting 25 ms after arming instead of
counting down for fourteen seconds. And the reconfigure now happens
after silence rather than through the middle of a track, which is what
the drain was for.

**The pattern worth naming: a condition that is a proxy for the thing
you mean.** `play != fill` was standing in for "the previous track is
still sounding", and the two agree often enough to look correct and
diverge exactly at the boundary being handled. 0718's `!s_playing` for
"a pause happened" was the same mistake one patch earlier.

#### The title waits for the cover (0721)

At a track change the screen changed twice: the new title against the
old cover, then the cover. The two are loaded on different tasks --
`load_tags()` on the decode loop at the change, `do_art()` on
media_task some way behind -- and each published as it landed.

Two changes read as a correction. The first frame looks like the player
got the title wrong and fixed it, or like the art is late for a track
that has already started; neither is what happened, and both are worse
than one change slightly later.

So the text is staged and shown when the art for the same track has
settled. `s_tags_shown` is what the UI reads; `s_tags` and
`s_display_name` remain what the loaders write, so every other site in
the file is untouched.

- **The UI task does the copy itself.** Publishing from media_task
  would be a struct written by one task and read by another with no
  ordering -- a torn title for a frame. The UI task is the only reader,
  so having it do the copy makes the question disappear instead of
  answering it.
- **`s_text_release` is set after `do_art()` returns, whatever it
  decided** -- a cover, a format card, or nothing because the
  generation moved on. The art strip is as settled as it is going to
  get, which is the actual condition, not "a cover was drawn".
- **`TEXT_HOLD_MS` is the escape.** `do_art()` can decline entirely:
  the chooser is up, the file would not open, the generation changed.
  None of those should cost the listener a title, so after a second the
  text shows regardless. The failure mode is a slightly late title
  rather than a missing one.

This is the third thing deferred to the moment it belongs to rather
than the moment it was ready -- after the envelope and the position,
which wait for the writer to arrive on this track's ring. The shape
keeps recurring because a decode loop that runs twenty seconds ahead of
the sound is always ready before the listener is.

#### The rate-change dip: the same seconds, spent in two halves (0720)

A crossfade cannot span a sample-rate change and never will here: there
is no resampler, mixing 44.1 into 48 is a nine percent pitch shift for
the length of the overlap, and reconfiguring the I2S clock disables the
channel anyway. 0719 made the refusal visible. This makes it sound like
something other than a cut.

**Not an overlap.** A ramp down over half the configured crossfade,
ending exactly where the outgoing ring does; then the drain and the
reconfigure; then a ramp up over the other half. The two never coexist,
so nothing is mixed and no rate is ever wrong. **The listener asked for
N seconds of softening and gets N seconds of it** -- what changes is
that the halves are sequential rather than simultaneous.

- **The down ramp is armed against the OUTGOING rate.** It is measured
  in frames of a ring still being clocked at the old rate, and using
  the new one would make it wrong by the ratio of the two -- 9% for
  44.1 into 48, which is the error this whole path exists because of.
- **It starts on the same countdown `xfade_can_start()` uses:** when
  the outgoing ring has come down to the length of the ramp. Once the
  decode loop has moved on, that ring can only shrink, so it is a
  countdown with a guaranteed direction rather than a race.
- **The up ramp is not armed by a countdown**, because there is nothing
  to count: the ring is empty, the clock has just been reconfigured,
  and the next chunk is the first of the new track.
- **The arming is cleared at the reconfigure whether it fired or not.**
  If the outgoing ring was already empty there was nothing to fade, and
  an arm left standing would ambush the incoming track: its own ring
  passes the same threshold near its end, and it would fade out in the
  middle of a track nobody asked to end.
- **Linear, not equal-power.** The crossfade uses an equal-power curve
  because two signals are summed and their powers add; a dip has one
  signal and silence, and there is nothing to preserve power against.

`FADE_OUT_MS` and its ramp are untouched. That one is for media that
went away -- it flushes the ring and blanks the screen behind it, which
is exactly wrong for a boundary where the next track is already
decoding.

#### It was not Ogg. It was 48 kHz (0719)

Two boundaries lost their crossfade -- Vorbis into Opus, and Opus into
AAC -- with no line in the log to say why. It read as Ogg misbehaving.

**Opus is 48 kHz and everything else in the suite is 44.1.** Both
boundaries were rate changes, `xfade_can_start()` refuses those (there
is no resampler, and mixing them is a pitch shift on one of the two
tracks for the length of the overlap), and it refuses them **without
saying anything** -- it is the writer, on the audio path, where a log
line per attempt would be a log line every few milliseconds.

0710 did add a line, but put it inside the drain branch, which only
runs when the rings have not yet converged. At a boundary where the
previous track had already played out there is nothing to drain, so the
fade stayed armed, the writer refused it silently, and the log showed a
crossfade that simply never happened.

The rate test is now its own decision, taken wherever the incoming rate
first becomes known, and it prints what it compared:
`no crossfade: 44100 Hz into 48000 Hz`.

**The pattern, third time in this series:** a refusal that is correct,
silent, and therefore indistinguishable from a fault. 0717 was a
recording that stored nothing; 0718 was a pause counted as a stall;
this is a fade declined for a good reason nobody could see. The
decision itself was right in all three. What was missing each time was
the sentence saying so.

Worth noting where such a line belongs: on the decode loop, which
decides once per track, and not in the writer, which asks per chunk.
That is why this fix moves the test rather than adding a log to
`xfade_can_start()`.

#### A pause inside a blocking send is an event, not a state (0718)

`W ring send blocked 30078 ms (ring 99%)`, logged at the instant the
listener pressed play again after a thirty-second pause.

The warning already excuses a pause -- `!s_playing` is part of
`running_ahead`, and it is sampled both before the send and after it,
because 0403 learned that testing only afterwards misses the state the
handoff has just cleared. **A pause that begins and ends inside a
single send is invisible to both samples.** Before it, playback is
running; after it, playback is running again; in between, the decode
loop sat in `xStreamBufferSend()` for the whole pause and reported it
as a stall.

`s_pause_epoch` counts pauses and is read either side of the send. A
counter can see an event; a boolean can only see a state, and the event
here is entirely contained in the gap between two reads of it.

Thirty seconds of "blocked" in a log is the kind of number that sends
somebody looking for a performance fault that does not exist -- which
is the same reason the tail and decode-ahead cases were excused in the
first place. **This warning has now been wrong three times in the same
way**, and each fix has been about widening what counts as "somebody is
waiting for this audio". It is worth asking, next time it fires
wrongly, whether the question should be inverted: warn when the WRITER
starved, which is one place and one fact, rather than when the decode
loop blocked, which is a dozen legitimate reasons and a growing list of
exceptions.

#### `long` is 32 bits here, and the guard was `at <= -1` (0717)

0716 recorded nothing. Fifteen complete plays of the ADTS file, each
logging `recording a table as it plays`, each storing zero pairs, in
silence.

The bound on a recorded offset was written `at <= (long)UINT32_MAX`.
**On this target `long` is 32 bits, so that cast is -1**, and the test
was `at <= -1` -- false for every real offset in every file. Not one
pair was ever appended.

The habit came from `mp4seek.c`, where the same comparison guards a
`uint64_t` chunk offset against truncation into a `uint32_t` field and
is genuinely needed. Here the value is already a `long`, so it cannot
exceed `UINT32_MAX` and the check was never a check.

**The silence is the more important half.** A recording that produced
zero entries failed the `> 1` test at the far end and said nothing, so
the log showed a feature starting fifteen times and never finishing,
with no line to say why. That case now warns. The rule, which this
project keeps rediscovering: **a path that decides not to do something
should say so.** `no crossfade: same file`, `adts declares VBR`, `the
seek ran off the end` -- every one of those exists because its silent
version wasted somebody's evening.

Worth checking the siblings when reading this: the same comparison in
`mp4seek.c` and `cbrseek.c` is against `uint64_t` values and is correct
there. Type, not habit, decides whether the guard means anything.

#### A table for the file that has nothing to build one from (0716)

Raw ADTS that writes `buffer_fullness = 0x7FF` in every frame header --
which is every file ffmpeg's AAC encoder produces -- declares itself
variable, `cbrseek.c` believes it, and the file then has **no** source
of a time-to-offset map: no proven rate, no container, no page granule,
no sample table. It sat in the seek bar's middle state, showing a
position it could not be dragged to.

Its frames are self-syncing, so the resume half was always free. The
missing half is where a second lives, and the only honest source for
that is **a play that watched it happen**:

- `decoder_stream_pos()` returns the offset of the next byte the
  decoder will consume -- `ftell()` minus the window's unread tail, so
  it is the byte being decoded rather than wherever the read pointer
  ran ahead to. Half a window of error here would put every later seek
  half a second past what the bar said.
- The decode loop records one `(offset, frames)` pair every
  `REPLAYGAIN_INDEX_SPACING_SEC` of **output**, not of wall clock: the
  decode runs ahead of the audio, and a table keyed to when the
  sampling happened would be keyed to nothing.
- On a later play the table is installed, the seek picks the pair at or
  before the target, and `cbr_adts_resync()` walks forward to the next
  real frame header -- exported from `cbrseek.c` rather than rewritten,
  because a raw AAC payload contains `FF F1` constantly and the
  chain-of-three validation is the part that matters.

**Costs nothing to record.** An `ftell()` every two seconds, on a play
that was happening anyway -- the same bargain as the loudness, the
envelope, and 0703's MP3 table. Nothing is scanned and nothing is read
twice.

**A seek during the recording ends it.** From that point the frame
count no longer counts the file from its start, so every later pair
would be wrong by the size of the jump. Same rule at the end: written
only from a play that reached the end without one, because a partial
table is worse than none -- a drag past where the recording stopped
lands on the last pair and reads as the press having been ignored.

**Narrow on purpose.** `decoder_needs_table()` is true only for a
stream that has been offered a proven byte rate, three bisections and a
sample table and matched none of them. It is the exception path, not a
general mechanism, and every format that has a real answer keeps using
it.

So the ADTS file needs three complete plays to become seekable: one to
learn its loudness, one to learn its length (0714), one to record its
table. That is the defeatist bargain stated plainly -- **nothing is
scanned, so everything is learned by listening.**

#### Repeat-one found the sidecar reading its own stale copy (0715)

0714 worked: `length from playback: 59 s`. Then the very next open of
the same file said `no duration available; seek bar will stay empty`.

**The write had not landed yet.** `rg_release()` hands the record to
media_task and the file is written a second or so later -- the log has
`length from playback` at 71627 and `sidecar written` at 72780, with
the next open in between at 71675. So `rg_hold()` read the version from
before the play that had just ended, and the measurement was replaced
by the stale copy it was on its way to replacing.

This has presumably always been true and was invisible because nothing
played the same file twice in a row until RPT existed. **A feature
added to make a bug reproducible found a different bug first.**

`rg_hold()` now takes the pending record when the path matches. That is
not a cache: it is the same record, one step earlier on its way to the
same place, and the file is the copy that is behind.

**And 59 was rounding, not error.** The count is frames that reached
the ring, a hair under a 60 s file once the last partial block is
accounted for, and truncation turned that into 59. Rounded now: half a
second on a bar 720 px wide is under a pixel, so the nearest second is
the honest report.

One ordering note for anyone moving this code: `s_rg_pending` and its
flag are now declared above `rg_hold()` rather than beside the writer,
because `rg_hold()` reads them. This file has been bitten by
use-before-declaration before.

#### The length was recorded in the one place it could not run (0714)

0712 added "measure the duration from a complete play" for files that
never state one, and put it inside the loudness block. That block is
gated on `measuring`, which is `!known` -- false once the sidecar
already holds a loudness and an envelope.

So the file whose length was still missing was **exactly the file that
no longer measured anything.** The board played `08 aac-adts.aac` from
start to finish with no seek, and logged no `length from playback` line
at all, because its loudness had been recorded two runs earlier. It
would have worked precisely once, on a card where that file had never
been played -- which is not a state anybody debugging it was ever going
to be in.

A length is not a measurement of the audio. It is a count of what came
out, so it belongs with the other facts about how the track ended and
depends on nothing but those: played to the end, no seek in it, a known
rate. It now sits next to `s_prev_ended_clean`, outside every
measurement gate.

The envelope's own span is filled from the same number in the same
pass, so the first complete play of such a file cannot write a waveform
spanning `0s` next to a format section that knows better.

**The general fault, worth naming because it is subtle:** a conditional
that is *usually* true is not a place to put something that only
matters when it is false. `measuring` and "has no duration" look
correlated -- both are about a file nothing has learned yet -- and they
are opposites in the case that matters, because one is cleared by
learning anything at all.

#### RPT: the fourth play order (0713)

`ONE` stops at the end of a track and there was no way to say "play
this one again", which is the mode a test suite wants most -- and the
one anybody debugging a single file wants. The order button now cycles
**ONE -> ALL -> RND -> RPT**, with the two single-track modes
bookending the two whole-folder ones.

`ONE` and `REPEAT_ONE` are opposites with almost the same name, so both
call sites say which they mean. Neither changes what the skip button
does: pressing next under either still moves on, because **a press is
not the end of a track**, and leaving RPT as-is would replay the track
the listener just asked to leave.

`playlist_next()` returns the current path without touching
`s_current`, so a repeat is an ordinary track change as far as
everything else is concerned -- and the pieces that need to know
already do. The sidecar is still held, the cover is still cached, and
0702's refusal to crossfade a track with itself was written for exactly
this case, months before there was a way to ask for it.

`playlist_peek_next()` returns the current path too, which makes the
prefetch a cache hit rather than a read.

One consequence worth knowing rather than fixing: the play history
fills with the same path, so `prev` twice under RPT lands back on the
same track. That is arguably what repeat-one means.

#### Three things the fourth run showed (0712)

**A three-second track under a ten-second fade is not played, it is
passed through.** The writer clamps a fade to the outgoing tail --
`tail was shorter` -- and nothing clamped it to the INCOMING track, so
the board ran a 10244 ms fade into a 3 s file and the file was
inaudible. It looked like the track had been skipped. The decode loop
now refuses to arm a crossfade into a track shorter than twice the
fade, so the incoming track gets at least as long at full volume as it
spent arriving. The length comes from the sidecar, which
`track_change_begin()` loaded a moment earlier; **a track with no
recorded length is not refused, because unknown is not short.**

**0710 stopped a fade that had not begun, and the board ran one that
had.** Clearing `s_xfade_armed` at a rate change does nothing to a fade
already under way: `no crossfade: the rate changes` was followed nine
seconds later by `crossfade cut short: the outgoing ring emptied`, and
the first sound arrived 9190 ms after the press. `s_xfade_active` is
cleared as well now. The general lesson, which this project keeps
paying for: **a flag that arms something is not the flag that stops
it.**

**A file that will never state its length can still be measured.** A
raw ADTS file declaring `buffer_fullness = 0x7FF` has no duration from
any of the four seek mechanisms, so its bar stayed a groove for ever --
the board played one twice and read `0s` from the sidecar both times.
But an uninterrupted play measures it exactly, which is the same trade
the loudness and the envelope already make: one complete listen buys
something the file would not say. Written only when the decoder had no
answer, because a stated length is a fact and this is an observation,
and only from a play with no seek in it -- it sits inside the
`measuring && why == TRACK_ENDED` block for exactly that reason.

##### Two things in that log that are not faults

- **The title appearing "early" on track 12 happens on every track.**
  When the decode of a track finishes there can still be twenty seconds
  of it in the ring, and `the finished track has played out; the screen
  is the next track's now` is the deliberate handover: the screen
  belongs to what is coming, the ring to what is going. Track 12 is
  simply the one with cover art big enough to notice.
- **`.m4a` does seek; `10 m4a-alac` does not.** 0711's run has
  `mp4: seek to 43s, landed 42s, sample 1851` on file 09. The four taps
  in this run landed during file 10, which is ALAC -- no `mp4a` sample
  entry, no remux, no seek, by design. Two files with the same
  extension and different answers is confusing, and the answer is in
  the open log either way: `mp4: aac ... seekable` or `sample entry is
  'alac' ... leaving it to the M4A parser`.

#### The seek table was right and the spacing was wrong (0711)

Third run, and the first with anything actually dragged. Every
mechanism landed: WAV exact, FLAC 47 -> 46, Ogg 38 -> 37 and 42 -> 41,
MP4 43 -> 42 sample 1851, TS 44 -> 43. Each one at or before the
target and within a frame or page, each one reporting where it landed
rather than what was asked. `0705`'s reopen-replay-resume works on
hardware: `+4307 B header` on Vorbis, `+137` on Opus, `+376` on TS,
`+42` on FLAC.

Two things wrong, both in 0703.

**The 1705 ms was the predicted cost, arriving on schedule.** 0703 said
a ten-second table would make a seek decode up to thirty seconds of MP3
and that the number should be watched. Watched: `decoder_read blocked
1705 ms`, twice. The spacing is now **two seconds**, which is six
seconds of forward decode and about a fifth of the time. 256 entries at
two seconds still covers eight and a half minutes before the doubling
takes over.

Readers take the spacing from the record, so this does not invalidate
what is already written -- but nothing would ever rewrite it either,
because an installed table is deliberately not harvested again. So the
harvest now makes one exception: **when minimp3 has since built its own
index on top of the installed one, the finer index wins.** That is
exactly the comparison `num_frames > installed count`, and it means a
sidecar written at ten seconds upgrades itself the first time anyone
drags in that track.

**And the log said the opposite of what the code did.** With
`MP3D_DO_NOT_SCAN` and no Xing header, `ex.samples` is zero -- minimp3
never counted the file -- and the existing line read `no index;
duration unknown, not seekable`. Three claims, two of them false: the
table was installed, the length came from the sidecar, and the seek
worked. A log that contradicts what the player then does is worse than
no log; the first board run of 0703 read as a regression when it was a
success.

##### Not a fault: the 12.9 s before track 14

*(0908 made the log say this itself. The section below stayed correct
for five patches and the question was asked again anyway, from a fresh
log, because the log line did not carry what this section knows. That is
the argument for fixing the line rather than the document.)*

`first sound 12966 ms after the press` at the 44.1 -> 22.05 kHz
boundary looks like 0710 failed. It did not: the crossfade was
correctly disarmed (`no crossfade: the rate changes`) and what remains
is the drain itself, waiting for 3519 KB of the previous track -- about
twenty seconds of audio -- to play out of the ring. **Nothing is silent
during it.** The measurement is from the press that started the track,
and the previous track is still playing; it is the same number the
gapless path would produce. Shortening it means a smaller ring, which
is the thing the ring exists to be large about.

#### A rate change and a crossfade cannot both happen (0710)

Second run of the suite, with 0709 applied. No crash, all fourteen
files handled, MP4 down from 2585 reads to 240. Two things left, and
one of them is the worst number in any log this project has taken.

**17.8 seconds from press to sound**, at the boundary from a 44.1 kHz
track to a 22.05 kHz one, ending in `crossfade cut short: the outgoing
ring emptied`.

The two mechanisms wait for each other. A rate change cannot happen
while the previous track is still playing out of the other ring -- the
reconfigure disables the I2S channel -- so the decode loop waits for
the rings to converge. A crossfade *keeps them from converging*: the
writer is mixing the outgoing ring against an incoming one that the
decode loop cannot fill, because the decode loop is in that wait. The
fade runs its full twelve seconds against silence, the outgoing ring
drains, and only then does anything play.

Neither is wrong on its own, and mixing two rings at different sample
rates is meaningless anyway, so the crossfade loses. It is disarmed at
the drain rather than at the arming block because **that is the
earliest the incoming rate is known** -- it comes from the first
decoded frame, which happens after the decision to crossfade was
already made.

**And the TS probe got 0709's treatment before it was measured rather
than after.** It walked packets one 188-byte read at a time, which is
the same shape as the MP4 reader and the same shape the arbiter
punishes: the board logged `60 reads, 69 KB in 1349 ms held`. Reading a
window of 64 packets at a time takes the real file's probe from 60
reads to 18, and a seek costs about eight.

That is now three separate places where a sequential small-read pattern
looked free on a host and cost real time on the device. The rule worth
keeping: **if a walk reads one record at a time, it is wrong before it
is measured.**

##### Still true after this run

`08`'s empty bar (ffmpeg declares VBR), `02` skipped (24-bit), `11`
reporting 59 s of 60, and the two mp3 opens at ~1.5 s -- which are
0703's first-play scans, and the one thing in the log that a second
play of the same file should fix. Nothing in this run replayed one, so
that is still unmeasured.

#### What the board found that the host could not (0709)

First flash of the whole series, against the `seektest` suite. Two
faults, and neither was in the seeking.

**A NULL dereference that had been latent for months.**
`sidecar_prime()` calls `mediacache_art(path, NULL)` purely to ask
whether art is cached; `mediacache_art()` wrote `*len` unconditionally.
It only faults when the answer is *yes* -- an entry that exists and has
art -- so it needed a file with embedded cover art already in the cache
to reach the store. The test suite has one, and it panicked on it:
`Store access fault` at `mediacache.c:158`, from `prefetch_next()`.

Nothing to do with 0700-0708. The suite found it because a folder of
deliberately varied files is a different thing from a folder of music
somebody happens to own.

**MP4 read one sample at a time and the board hated it.** The host said
nothing: the samples are contiguous, `fseek()` was skipped, and the
stdio buffer absorbed the reads. The board counted them. A sixty-second
track logged **2585 reads for 962 KB, 1545 ms of arbiter hold, and
`decoder_read blocked 959 ms`** -- against 43 reads for the same audio
as Ogg. The cost is not the bytes, it is two and a half thousand lease
acquisitions on the decode loop.

`mp4_read()` now measures the run of samples that fits in the buffer,
reads it in one call, and spreads it in place to make room for the
headers -- safe in ascending order because each sample moves back at
most `7*N` bytes and every earlier one moves further than the one after
it. Same file: **126 reads instead of 2585.**

The general shape, which is the third time this project has met it: a
pattern that a buffer hides on a desktop is a pattern the arbiter
counts on the device. `storage_io`'s per-call accounting exists to make
that visible, and it did.

##### Three things in that log that are working as intended

- **`08 aac-adts` shows no duration and no bar.** ffmpeg's AAC encoder
  writes `buffer_fullness = 0x7FF` in every ADTS header, which is the
  stream declaring itself VBR, and `cbrseek.c` takes it at its word:
  `adts declares VBR (buffer_fullness 0x7FF)`. No proven byte rate
  means no derived duration, and no duration means an empty bar. The
  refusal is the feature; the empty bar is what refusing looks like.
- **`10 m4a-alac` would not play**, with `M4A_PARSE: Not support mdat
  before moov`. That is the parser's restriction, and the file was
  muxed without `+faststart`. Note which way it cuts: **09 played
  fine** with the same layout, because 0707 reads the tables itself and
  does not care where `moov` sits.
- **`08`'s 1314 ms open is contention, not parsing.** Its own I/O line
  says 12 reads, 69 KB, 11 ms held. The previous track's ReplayGain
  sidecar was written at the same moment, on the media task. The open
  path was waiting for the card, not working.

#### What real files found that synthetic ones did not (0708)

Every seek in 0700-0707 was tested against files this project generated
itself, and all of them passed. The first run against ffmpeg output
found two faults in ten seconds, both in code that had passed hundreds
of synthetic cases and four hundred mutations each.

**FLAC: a drag to the end of a track landed at 0:13.** `find_frame()`
was written as "confirmed unless the following header disagrees", with
the confirmation flag initialised to true -- so a candidate with no
following header in the window was accepted unexamined. The tail of a
real file is exactly where that happens. A 445-byte run of audio data
at the end of the file passed the CRC-8, had nothing after it to
contradict it, and declared a sample number 45 seconds out of place.

Confirmation is now required rather than preferred. The cost is that
the genuinely last frame can never be confirmed and so is never landed
on: one frame, 93 ms, at the very end of a track, erring towards
playing slightly more.

**Ogg: every seek landed two seconds early.** The bisection kept the
last page ending at or before the target, and a granule position is
where a page *ends* -- so the audio resumed where the page before that
one ended, two pages back. On synthetic files with small pages the
error was under the tolerance and invisible. ffmpeg writes Vorbis and
Opus pages of roughly a second, and two seconds is not invisible at
all.

It now takes the first page ending *after* the target, so the audio
resumes where the page before it ended: at or before the target and
within one page. Half the error and on the correct side.

**The lesson is about the test data, not the bugs.** Both faults were
in the tolerance between "a plausible file" and "a file an encoder
actually writes" -- page sizes an order of magnitude larger than
assumed, and a tail that real muxers produce and synthetic generators
do not. Generated test files verify the logic against the format as
understood; they cannot verify the understanding. `seektest/` exists so
that the next mechanism is measured against ffmpeg's output before it
is called done.

Two things the suite documents rather than fixes, because they are
correct: ffmpeg's AAC encoder writes `buffer_fullness = 0x7FF` in every
ADTS header, which is the stream declaring itself VBR, and `cbrseek.c`
takes it at its word and refuses -- so raw ADTS from ffmpeg does not
seek. And `.ts` reports a duration one second short, because the last
timestamp is the start of the last packet rather than the end of the
audio.

#### MP4 is the one where the archive said no (0707)

Every seek from 0700 to 0706 works by moving the file underneath
esp_audio_codec's parser, which is allowed because those parsers are
stateless about position -- they find their own boundaries in whatever
arrives. `m4a_parse.c.obj` is not. It is 6652 bytes and its strings say
what it does: `Chunk number %d`, `Sample number %d`, `STSC map count
%d`, `Fail to allocate memory for stco` / `stsz` / `stsc`, `All sample
sent`. **It reads the sample tables into memory at open and walks
them**, driving position itself and telling the caller which bytes to
skip.

So there is no position to move it to. Reopening restarts it at sample
zero and feeding it bytes from elsewhere desynchronises it against a
table it believes it is tracking.

**So this one stops using it.** `mp4seek.c` reads the same tables --
`stts` for timing, `stsc`/`stco`/`stsz` for where each sample is,
`esds` for the AudioSpecificConfig -- synthesises an ADTS header per
sample from that config, and feeds the AAC decoder, which takes
arbitrary-length input and resynchronises on its own sync word.

The remux is fifteen lines because the AudioSpecificConfig is five bits
of object type, four of sampling frequency index and four of channel
configuration, and those are exactly the three fields an ADTS header
carries. It is a reframing, not a transcode.

**It is the only exact seek in the player, and the only one that is a
lookup rather than a search.** The table says where every sample begins,
so there is no bisection, no preamble, and no reopen -- every frame
handed over carries its own header, so a jump is the next frame coming
from a different sample.

##### What it declines, and why that is the design

Anything in an MP4 that is not AAC or ALAC. Every reason `mp4_probe()`
can fail lands on esp_audio_codec's M4A path, working and unseekable --
a sample entry that is neither `mp4a` nor `alac`, a missing or unusable
`esds`, an escape-coded sample rate ADTS cannot express, more than
`MP4_MAX_SAMPLES`, an offset past 4 GB.

**ALAC was on that list until 0808 and is not any more.** It has no ADTS
framing and never will, so it is not remuxed: it is handed to
`_ALAC` a frame at a time, which that decoder accepts and which only
the sample table can supply. Same table, same lookup, different decoder
-- and two further ways back to the fallback, both logged: the magic
cookie refused, or no room for a buffer the size of the largest
sample.

Putting the decision in a probe rather than in the extension table is
what makes that fallback free. The format table still says `.m4a` is
M4A; the probe upgrades it to AAC when it can.

The other seek probes are not asked at all once this one has the file.
Two mechanisms claiming the same file is two mechanisms that can
disagree.

##### Details

- **`stsc` is runs, and its last run covers every remaining chunk.**
  That is why the expansion cannot be one loop: the final entry has no
  successor to bound it, and writing it as though it did leaves the
  tail of the track unplaced. The check is that the number of samples
  placed equals the number `stsz` declared, and a disagreement rejects
  the file rather than playing part of it.
- **Buffer fullness is written as 0x7FF**, meaning variable. That is
  the truth about a remuxed stream, and it is also exactly what
  `cbrseek.c` reads as a refusal to treat a stream as constant-rate.
  Both are right, and they agree with each other by accident of both
  being honest.
- **HE-AAC and PS signal the base object type**, which is what implicit
  signalling means and what every ADTS remuxer does. Scalable, ER and
  USAC configurations are refused instead of being written as something
  they are not.
- **`MP4_MAX_SAMPLES` is 200000**, which is 1.6 MB of PSRAM and 77
  minutes at 1024 samples and 44.1 kHz. Past it the file gets the
  fallback rather than an allocation nobody budgeted for.
- **A contiguous run of samples costs no seeks.** The reader tracks
  where the handle is and only calls `fseek()` when the next sample is
  not where it left off, which for a normally-muxed file is never --
  the samples are in order in `mdat` and the stdio buffer does the
  rest. A seek sets the position to -1 so the next read cannot mistake
  a jump for a continuation.

Host-tested under ASan and UBSan: synthetic files at one, seven and
thirteen samples per chunk and with both `stco` and `co64`, every
sample's computed offset and size checked against the generator's,
every second seeked with the resulting ADTS header decoded back and the
payload compared byte for byte; then 400 mutated cases per file. Not
IDF-built.

#### TS seeks on a lattice (0706)

`tsseek.c`, and the archive was read first again. `ts_parse.c.obj` is
2894 bytes of RISC-V: it compares against the 0x47 sync byte in nine
places, parses PAT and PMT, filters by PID and reads PES headers -- and
it contains exactly one four-bit mask, which is the PSI section
length's high nibble. **The continuity counter is not tracked.** That is
the field a demuxer would use to notice a jump, and it does not look at
it.

**The lattice makes this the easiest of the four seeks.** A transport
stream is fixed-size packets at a fixed stride, so every candidate
offset is `base + n * stride` and there is no resynchronisation to get
right and no confirmation to construct -- the sync byte at the computed
place is the confirmation. Three strides are handled: 188, 192 (m2ts,
which prefixes a four-byte arrival timestamp) and 204 (188 plus
Reed-Solomon parity). They are told apart by checking five sync bytes,
not by parsing anything.

**PAT and PMT are the preamble.** Without them a fresh parser does not
know which PID carries audio or what codec is in it, so it would filter
for a PID nobody has told it about. Two packets, replayed verbatim --
Ogg's header pages, WAV's synthesised header, and now this: the third
instance of the same shape, and the reason `decoder.c` has one queueing
mechanism for preambles rather than two.

**Non-monotonic timestamps are refused, not searched.** PTS is 33 bits
at 90 kHz, so it wraps every 26.5 hours, and a stream spliced from two
sources can restart it part way. A bisection needs a key that
increases; over one that does not it **does not fail, it converges on
the wrong packet**. So the probe checks that the last timestamp is
after the first and declines the whole file otherwise -- no seek, and
no duration either.

**It also gives .ts a duration**, which `duration.c` never could: there
is no header stating one, only the span between the first and last
presentation timestamps, and the seek probe has already read both for
its own clamp.

Two smaller decisions:

- **A stream type this player does not recognise is still tried** if it
  is the only elementary stream in the programme. The decoder will say
  so if it cannot read it, and refusing at the PMT would cost a seek on
  a file that plays.
- **PSI sections spanning packets are not reassembled.** A PAT is four
  bytes of payload and a radio PMT a few dozen; both fit in one packet
  in any stream this will meet, and a reassembler is a parser for a
  case that does not arise.

Host-tested under ASan and UBSan: synthetic streams at all three
strides, with and without an interleaved video PID putting hundreds of
packets between audio PES headers, every second seeked and checked to
land on a real audio PES start with the clock within a second of the
request; then 400 mutated cases per file. Not IDF-built.

#### Ogg seeks too, and the archive is what settled it (0705)

`oggseek.c`. Same bisection as FLAC, over page granule positions
instead of frame headers.

**This was deferred twice on a question that could have been answered by
reading the binary.** The doubt was whether esp_audio_codec's Ogg parser
would accept pages from a new position: a demuxer is entitled to treat a
page sequence number that jumps as a hole and to drop pages or fail, and
the component ships as a precompiled archive, so the header says nothing.
The recommendation both times was to flash a test.

`esp_ogg_parse_frame` in `libesp_audio_simple_dec.a` is 1232 bytes of
RISC-V, and disassembling it answers the question outright:

- it **scans forward for `OggS`** anywhere in the buffer it is given and
  reports the skipped bytes rather than failing, so resynchronisation is
  a supported operation;
- it checks the version byte and compares the **serial number** against
  the one learned at the start of the stream;
- it **never reads the page sequence number at offset 18, and never
  reads the CRC at offset 22.** There is no CRC table in the object --
  the only `.rodata` in it is format strings.

So a sequence-number jump is invisible to it. Half a day of flashing
replaced by twenty minutes with `nm` and a disassembler, on a question
that had already cost two rounds of "it would take an experiment".

**Ship's-log note, since this is the second time it has come up:** a
precompiled dependency is not a black box. `nm`, `strings` and a
disassembler answer questions about it that its header does not, and
this project already has to know things about esp_audio_codec that are
not documented -- the MP3 symbol collision in `minimp3_prefix.h` was
found the same way.

##### The headers still have to be replayed

The disassembly also shows the two pieces of state a jump invalidates: a
flag saying the beginning-of-stream headers have been parsed, and a
partially-assembled packet that `append_packet` splices across pages. So
this is 0701's shape again -- close the decoder, reopen it, replay the
stream's own header pages verbatim, then the pages at the target. Not
because the sequence numbers need fixing, but because the parser's
packet assembler is mid-packet and its header state would be missing.

Verbatim rather than synthesised: real pages carry correct CRCs, and
although this parser does not check them, the next version might, and a
synthesised page is the kind of thing that works until it does not.

**Header pages are the pages at the front whose granule position is
zero.** Vorbis has three header packets and Opus two, and neither can
have produced samples yet, so the first page with a nonzero granule is
audio. Far more robust than counting packets, which for Vorbis means
walking a segment table across page boundaries to find where the setup
header ends.

**Vorbis is why the preamble is a source rather than a memcpy.** Its
codebooks run to several KB and can exceed the decoder's 8 KB input
window, so `decoder.c` grew `pre`/`pre_len`/`pre_pos` and fills the
window from the preamble before the file. WAV's 44 bytes and AMR's six
still go straight in.

Details:

- **Opus granule is always 48 kHz units and includes the pre-skip**, and
  `OpusHead` carries an input-rate field that invites the wrong divisor.
  Same trap `duration.c` documents; same answer, in a second place
  because these are two different questions about the same number.
- **A page that continues a packet is not a landing site.** The parser
  would be handed the tail of a packet whose head it has never seen. A
  continuation page is still good evidence about position, so it still
  moves the interval -- it just cannot be the answer. Same for a granule
  of -1, which means no packet finishes on the page.
- **The landing page's own granule is not where the audio resumes.** A
  granule is the position of the END of the last packet finishing on
  that page, so resuming there produces audio starting where the
  PREVIOUS page ended. Reporting the landing page's granule would put
  the clock up to one page ahead of the sound -- 20 to 200 ms,
  permanently, for the rest of the track. One extra read of the window
  before the landing page gets the predecessor's granule, and that is
  what `decoder_seek_sec_at()` reports.
- **The window is 24 KB with a 65 KB fallback.** A page can be 65307
  bytes, so a window that always guaranteed a page start would be a
  megabyte of reads per drag. 24 KB covers five or six typical audio
  pages; the full size is the retry for a probe that found nothing.

Host-tested under ASan and UBSan: synthetic Vorbis and Opus streams with
multi-page headers and variable page sizes, every second seeked and the
result checked to be a real page start with the clock within one second
of the request; then 400 mutated cases per file. Not IDF-built, and the
decoder handoff -- reopen, replay, resume -- is the part no host test
can reach.

#### A FLAC with a big cover never reached its audio (1009)

An album of 24-bit FLACs played nothing, three tracks in a row, and the
player correctly gave up:

```
flac: 48000 Hz, 2 ch, 24 bit, audio at 1876654, 9343683 samples
E ESP_ES_PARSER: Search overlimited 512000
E tab5_dec: flac decode error -7
E tab5_mp3: 3 tracks in a row played nothing; stopping.
```

**esp_audio_codec's elementary-stream parser searches a bounded window
for a frame sync and gives up past it.** The limit is 512000 bytes.
These files carry an embedded cover, so their audio starts 1876654 bytes
in, and the sync is three and a half times further than the parser will
look. The I/O line confirms it to the byte: `68 reads, 512 KB` -- the
search limit exactly, then failure.

Nothing was wrong with the files, the fold, or the 24 bits. `21
flac-24bit.flac` in the corpus plays, and its audio starts at 8288.
**The corpus has no file with a large cover, so it could not find
this** -- the same gap 0808 recorded when synthetic files missed what
ffmpeg's output caught, one step further out: a real file with a real
picture in it.

**The fix is machinery that already existed.** `flac_seek_probe()` reads
STREAMINFO and records `first_frame` at open, and since 0704 a seek has
replayed a synthesised header in front of the target -- `"fLaC"`, a
last-block header, and the 34 bytes of STREAMINFO, 42 in total. That is
applied at open now: position the file at `first_frame`, put the
preamble in the window ahead of it. The parser sees a header and then a
frame, which is what the front of a file with no pictures in it looks
like.

**Unconditional rather than gated on a size.** A threshold would leave
two open paths differing only on files most people do not have, which is
the second path `flacseek.c` declines to have for the SEEKTABLE and for
the same reason. It is also precisely what a seek to 0 s already did,
and that is proven on hardware: `flac: seek to 0s -> offset 8288, landed
0.00s (+42 B header)`. Anything that fails -- no preamble, a refused
`fseek()` -- falls back to reading from the top, which is what every
FLAC did before.

The refill path needed no change and that is worth knowing rather than
rediscovering: it keeps `in_len - in_pos` bytes, moves them to the front
and appends the file after, so a primed window is preserved rather than
dropped. That is the same mechanism the seek path has always relied on.

**A side effect worth having:** opening at the audio also means the
1.8 MB of metadata is never read. The cover still arrives, through
`covertag.c`, on the task that is supposed to fetch it.

#### FLAC seeks by bisection (0704)

`flacseek.c`. FLAC is the opposite problem from the CBR formats and has
the better answer.

There is no line to prove -- a silent passage costs a handful of bytes
and a dense one costs thousands -- so `cbr_probe()` is unavailable by
construction. But **every FLAC frame header carries the sample it starts
at, and a CRC-8 over itself**, so the position of any byte in the file
can be *read* rather than estimated. Finding a target is a binary search
over byte offsets: about fifteen probes of a few tens of KB, at the
drag, with nothing read at open beyond STREAMINFO.

The result is the most accurate seek in the player. cbrseek lands within
a tolerated drift; this lands on the frame that contains the sample
asked for, and says which sample that turned out to be.

**The SEEKTABLE is deliberately not read.** It is optional and plenty of
encoders omit it, so a mechanism built on it needs the bisection written
anyway for the rest; its points are typically ten seconds apart, which
is a coarser answer than the bisection gives and would have to be
decoded through -- the cost 0703 accepted for MP3 and there is no reason
to accept here. A file that has one is simply ignored, which costs
nothing and removes a second path exercised only on some files.

**The CRC-8 is what makes the resync reliable rather than a heuristic.**
FLAC audio data is high-entropy and `FF F8` appears in it constantly --
the same trap an ID3 tag full of PNG sets for MP3 sync scanning. The
header's own CRC turns "looks like a header" into "is a header" with a
one-in-256 residual, and the field checks against STREAMINFO plus a
confirmation that the *next* header's sample number is exactly this
one's plus its block size take that the rest of the way. A candidate
that cannot be confirmed is skipped.

**Fixed blocking counts frames; variable blocking counts samples.** One
bit in the header says which, and getting it wrong scales every position
by the block size -- a few thousand times out. The search would still
converge, on the wrong answer, because it only requires the numbers to
be ordered. Both are generated and tested.

**The bisection is bounded at 24 rounds rather than run to
convergence.** What makes a bisection terminate is the interval
shrinking every round, and this one shrinks by landing on a header whose
position is decided by the data. On the decode loop, a bound is worth
more than a proof.

##### A seek reports where it landed

`decoder_seek_sec_at()` exists because this is the first mechanism whose
answer is not the question. minimp3 decodes forward to the exact sample;
the CBR path lands on the first frame at or after the target; the
bisection lands on the frame *containing* it, which starts up to 93 ms
before. Small, and not zero.

`player.c` re-anchors `frames_out` and `s_pos_sec` from a seek, so
anchoring to what was asked for rather than to what was reached puts the
clock permanently out of step with the audio by the width of a frame --
an error that never corrects itself, on the one control whose whole
purpose is to agree with the position. The landed value is logged when
it differs, so the size of the gap is visible rather than assumed.

Host-tested under ASan and UBSan: synthetic streams in both blocking
strategies, with and without a 500 KB PICTURE block ahead of the audio,
random high-entropy payload throughout (which does contain false syncs
-- the confirmation is what rejects them), every second of each file
seeked and checked against the generator's own frame offsets; then 400
mutated cases per file. Not IDF-built.

#### Two things a crossfade must not do (0702)

0701 removed the decode error. It did not remove what the decode error
found, which is that the crossfade will happily mix a track with itself
and had two ways to be asked to.

**A seek that ran off the end is not a track that ended.** The arming
block said so in words already; nothing made it true. A drag to the last
inch of the bar leaves under a second of audio, the decoder reaches the
end of it at once, and `why` is `TRACK_ENDED` -- indistinguishable, from
the bottom of `play_file()`, from a track played through. It is not the
same boundary: the listener just moved the playhead themselves, so what
follows is a consequence of a press and should sound like one. The test
is one second of decoded audio after the last serviced seek. Below it
the seek ended the track; above it the track ran on and ended on its own
terms.

`why` itself is deliberately not changed by that test. It is the
caller's instruction about what to play next -- `TRACK_ENDED` advances
the playlist and `TRACK_INTERRUPTED` does not -- and a seek to the last
second of a track still wants the next one. Only `s_prev_ended_clean`
moves.

**Never over itself.** Repeat-one, or a `next` that wraps a one-track
folder, hands `play_file()` the file that is still playing out of the
other ring. An overlap of a recording with itself three seconds out of
phase is not a transition, it is a flanger. The rings cannot tell the
difference -- same rate, same channels, both full -- so the check is on
the path, next to the same-album one, and `s_prev_path` exists for it
where `s_prev_dir` already existed for the album.

Both are policy about how the previous track ended, which is why they
sit in the arming block on the decode loop rather than in
`xfade_can_start()` on the writer. The split is unchanged: the decode
loop decides whether an overlap is *allowed*, the writer decides whether
one is *possible*.

### Two volumes, mounted together

`storage.c` owns the microSD slot and the USB-A port, and both are mounted
at once rather than one being picked over the other. The chooser needs to
show a tab per slot and grey the empty one, which is not a question
"which filesystem is active" can answer.

**USB5V\_EN is P3 of the expander at 0x44**, not a GPIO, and not the
`EXT5V` on expander 1 that `PI4IOE1_OUT_SET` already drives. Until it is
high the USB-A port is electrically dead: the host stack installs, the
class driver registers, and nothing ever enumerates, with no error
anywhere. Three registers in this order -- direction, out of high-Z, then
drive -- and the high-Z one is the easy one to miss, exactly as it is for
`SPK_EN` on the other expander.

`PI4IOE2_IO_DIR` was already 0xB9, whose bit 3 is what distinguishes it
from M5Unified's 0xB1; that value puts P3 back to an input and the port
stays dark. `storage.c` re-writes the direction bit anyway rather than
depending on that constant keeping its value.

Card presence is polled at 1 Hz, because it has to be: the microSD
connector's detect switch is not wired to the SoC on this board -- M5's
BSP passes `GPIO_NUM_NC` for it -- so there is no edge to interrupt on.
`sdmmc_get_status()` is the removal signal, and an empty slot answers it
by timing out, which is why the poll is a second rather than faster.

A failed mount has to tear the host down (`sdmmc_host_deinit()`) or the
next attempt reports `conflict found for GPIO[42]`. That was already true
and already handled; it matters far more now, because the poll retries
forever rather than once at boot, so a leaked host is a guaranteed failure
a second later instead of a one-off.

### The USB port is powered at boot

It used to come up only when there was a reason: no card at boot, or the
USB tab tapped in the chooser. Both of those are questions about **where
the files are**, and that was the right gate while mass storage was the
only thing on the bus.

It stops being right the moment audio is on it. A USB audio device is not
a file source, and it cannot announce itself through a dark port -- so
with a card in the slot and nobody in the chooser, a headset plugged into
this player was invisible for as long as the card kept working. "Highest
priority output" is not implementable on a port you only switch on when
you go looking for music files.

So `app_main()` calls `usbhost_start()` unconditionally. What is lost is a
milliamp or two on a board with nothing plugged in -- which is the state
the port was in anyway. It is still one-way: cutting VBUS again would yank
a mounted drive or a playing headset out from under whatever is using it.

**`usbhost.c` owns the bus, not `storage.c`.** There are two class drivers
on it now and exactly one host stack and one VBUS enable underneath them.
Class drivers register before the port comes up and are installed in
registration order, between `usb_host_install()` and VBUS -- which is the
ordering `storage.c` already documented, and it matters for the same
reason: a device already in the port enumerates the instant power arrives
and should meet a stack that exists.

Two rules in that file are worth not undoing:

- **Registration after the port is up is refused, not honoured late.** A
  class driver installed after enumeration is never offered the devices
  already attached, so it would sit there looking installed and seeing
  nothing until somebody unplugged and replugged.
- **One class failing does not take the port down for the others.** A
  build where the MSC driver cannot allocate should still play a headset.

`USB5V_EN` is still P3 of the expander at 0x44, still needs three
registers in the order direction, out-of-high-Z, drive, and the high-Z one
is still the easy one to miss. That note moved with the code.

### A greyed tab is still a button

Grey means "nothing here yet", not "not a button". The underline is drawn
for the selected tab whether or not anything is mounted -- in grey rather
than red when it is empty -- because a strip with no underline at all
reads as a lost tap.

This used to be structural: the port was only powered by selecting its
tab, so the tab **had** to be selectable or there was no way to ask. That
reason is gone with the on-demand power. What is left is the ordinary one
-- a tab that ignores taps while a drive spins up reads as a lost tap --
so the behaviour stays and the justification is weaker. Do not treat it as
load-bearing any more.

The path row carries the reason instead of a path:

| State | Row reads |
| --- | --- |
| No card | `no card in the slot` |
| USB tab, port up, no drive | `USB port on - waiting for a drive` |

`storage_usb_powered()` still reports the *request* rather than the
completed bring-up, so the third state -- `USB port coming up` -- exists
only for the few milliseconds between `usbhost_start()` and VBUS going
high at boot. Nobody can tap their way into it any more.

## ReplayGain, and where the waveform comes from

Both come off the PCM the decode loop is already producing for the
speaker. That is the whole design: the expensive step is the decode, it
is already happening, and everything here is arithmetic on a buffer that
is already in cache.

### The measurement

`loudness.c` implements ITU-R BS.1770-4 integrated loudness as
ReplayGain 2.0 uses it: K-weighting, 400 ms blocks at 75% overlap, an
absolute gate at -70 LUFS, then a relative gate 10 LU below the mean of
what survived.

**The gating is the part that makes this not a running RMS with a better
name.** A track with a quiet intro and a loud body should report the
loudness of the body; an ungated mean reports something in between that
matches neither. Proven rather than asserted: 10 s of -40 dBFS followed
by 10 s of -20 reads -20.016 LUFS, not the -23 an average gives.

**The K-weighting filter is derived from the standard's own prototype
with `tan()` pre-warping, not from an RBJ-cookbook shelf fitted to the
same corner and Q.** This cost a round trip and is the single most
important thing in the file to not "simplify". The cookbook shelf
compiles, runs, produces plausible output, and lands 0.4382 dB at 1 kHz
where the standard's filter lands 0.6977 dB -- so every measurement came
out 0.25 LU low, which is outside EBU R128's +/-0.1 LU tolerance and is
invisible from the output alone.

The test that settles it, and the one to re-run if this function is ever
touched: **at fs = 48000 the derivation must reproduce the coefficient
table printed in BS.1770-4 to fourteen decimal places.** Not "the numbers
look reasonable". A 1 kHz sine at -20 dBFS stereo then reads -19.950
LUFS, inside tolerance, the residual being histogram quantisation.

Peak is **sample peak, not true peak** -- no 4x oversampling. Documented
as a simplification rather than left to be inferred from a better name.
Changing it is a `LOUDNESS_VERSION` bump.

### The envelope

Peak magnitude per column, accumulated in the same pass. The accumulator
sees blocks, not a file, so it never knows the duration and cannot size
its columns up front. Instead a column covers a fixed span; when the
array fills, adjacent pairs are merged and the span doubles. Any length
lands between 360 and 720 columns at one pass over 720 bytes per
doubling -- nine merges for a ten-minute track.

Merged with max, not mean, for the reason the frame walk gave and got
right: a mean turns a transient into a bump, and the transient is what
makes one track's shape recognisable.

The column still being filled is left out of the result. It covers less
time than the others, so its peak is drawn from a smaller sample and
reads low -- a dip at the right edge of every track, for one column.

### Applying it

`replaygain_gain_db()` is REFERENCE minus the measurement, **held back on
the positive side only** so the stored peak cannot be pushed past full
scale. Turning a loud track down never clips, so a negative gain applies
in full; a positive one is cut to whatever headroom the peak leaves and
the track ends up quieter than the reference.

That is the honest failure. A limiter would reach the target by squashing
peaks, and a music player has no business rewriting a waveform to hit a
number. It is also what the peak is stored *for*, rather than as a
curiosity beside the loudness.

#### Nothing digital can clip here, so 1003 logged instead (1003)

`peaking?` sat in the open list for a long time and 1003 was going to be
the limiter. Reading the path first says a limiter would have nothing to
limit, and three things combine to make that true:

- **`replaygain_gain_db()` caps a positive gain at the headroom the
  measured peak leaves**, minus `REPLAYGAIN_HEADROOM_DB` (1.0 dB). A
  gain can never push a peak past -1 dBFS. That is the section above,
  and it is the whole reason the peak is stored.
- **The gain loop saturates anyway**, at +/-32767 -- which, given the
  cap, is unreachable.
- **Volume is attenuation on both routes.** The USB software path scales
  linearly at or below unity; the analog path writes `LOUT1VOL`,
  `ROUT1VOL`, `LOUT2VOL` and `ROUT2VOL`, which is **analog attenuation
  after the DAC**. Turning the volume down does not reduce what the DAC
  is handed.

So a digital limiter's only effect would be to pull down content that is
legitimately at full scale -- **exactly the "rewriting a waveform to hit
a number" this file refuses one section up.** It would have closed the
entry by contradicting the argument above it.

**What the entry actually describes is analog**, and its own wording
says so: what the ES8388 output stage does on clipping. Because volume
attenuates after the DAC, the DAC sees full scale at every volume
setting, so that question is not answerable from the sample path at all.

**This is the cyan flash, caught one patch earlier than last time.** Six
patches throttled bandwidth against a DSI underrun mechanism that was
never running, and the note flagging the missing measurement as "the
single highest-value next step" then went five more patches without
taking it. The same move here would have been a limiter for clipping
nobody had observed, in a path that cannot produce it.

So 1003 is the measurement:

- **`output peak N/32768 (X dBFS)`**, once per track, taken **after**
  the gain rather than before it, because what is in question is what
  the DAC receives. `-- at the rail` is appended at full scale, which is
  the state the entry claims lossless files reach and MP3 does not.
- **Folded into the gain loop where that runs**, so it costs nothing on
  those tracks; a separate pass at unity, which is every unmeasured
  track and every one whose gain came out at 0 dB. Leaving those out
  would have measured only the half least likely to be at full scale.
- **`clamp_hits` is a warning, not a statistic.** It should be zero
  always, by the cap above. If it fires, either the cap is wrong or the
  stored peak disagrees with the audio, and both are claims about the
  code rather than about the file -- so it prints as `W` and says what
  it contradicts.

The magnitude runs 0..32**768**, not 32767: `INT16_MIN` is a real sample
value and `-(-32768)` does not fit in an `int16_t`, so it is negated as
an `int`. Verified under UBSan rather than reasoned about.

##### And the format claim was false (1004)

Run, in one session:

| Track | Peak | dBFS |
| --- | --- | --- |
| `03 mp3-cbr-noxing` | 15517 | -6.49 |
| `04 mp3-vbr-xing` | 16032 | -6.21 |
| `05 flac` | 15992 | -6.23 |
| `06 ogg-vorbis` (interrupted) | 16396 | -6.01 |
| `21 flac-24bit` | 15992 | -6.23 |
| `01 wav-pcm16` | 15992 | -6.23 |

**The lossless files are not louder.** The whole spread across five
formats is 0.48 dB and the MP3s sit inside it rather than below it --
`05 flac` against `04 mp3-vbr-xing` is 0.02 dB. `peaking?` had asserted
the opposite since it was written, and a limiter built on it would have
been solving a difference of two hundredths of a decibel.

**A result nobody asked for, from the same numbers.** `01 wav-pcm16`,
`05 flac` and `21 flac-24bit` all report **exactly 15992** -- the same
master through PCM, lossless 16-bit and a 24-bit fold, landing on the
identical sample. That is an independent check on `fold_24_to_16()`
(0803) that no test was written for: a wrong shift there is a factor of
256 and would have been unmissable in this column.

**But the corpus cannot answer the question the entry was really
asking.** All twenty-one files are one minute of the same source audio
transcoded, so they are an excellent test of whether *format* moves the
peak -- it does not -- and no test at all of whether real music reaches
full scale, because the absolute level is a property of that one master
and it sits at -6 dBFS. This is 0800's own lesson somewhere new: **a
test whose wrong answer equals its right one is not a test.** The corpus
was built to exercise mechanisms, and every file inheriting one master
is what makes it useless for a question about levels.

**One real track, and it is not conclusive either.** A 256 kbps MP3 off
the SD card -- Advent Chamber Orchestra, Eine Kleine Nachtmusik --
reports 23222, or **-2.99 dBFS**: 3.2 dB above everything in the corpus
and still 3 dB short of the rail. It is one file, and it is chamber
classical, which is the genre least likely to be limited to 0 dBFS. Its
own envelope says so: levels **1..181** against the corpus's 68..124,
which is the dynamic range a loudness-war master does not have. It was
also played at unity -- the ReplayGain switch was thrown mid-track, for
the *next* track -- so the figure is the file's own peak and no gain
path was exercised.

So the entry splits, and only half of it survives:

- **"Lossless arrives at full scale where MP3 does not"** -- refuted,
  struck.
- **"Does anything in a real library reach the rail"** -- still open,
  one classical data point at -3 dBFS, and not answerable from
  `test_audio_files/` at all.

The instrumentation is permanent, so the second question now answers
itself as the library gets played rather than needing an experiment.
**`clamp_hits` has still never appeared**, which is the prediction that
matters most: it is the one that would mean the headroom cap is wrong.

##### And it answered itself two patches later (1006)

A game soundtrack, 320 kbps MP3, first play:

    loudness: -5.53 LUFS, peak 0.00 dBFS, 2282 gated blocks
    output peak 32768/32768 (0.00 dBFS) -- at the rail

**So real music does reach full scale**, and the reason the corpus and
the classical track did not is now clear: the corpus is one master at
-6 dBFS, and chamber classical is the genre least likely to be limited.
A modern loudness-war master is pinned at 0.00, which is exactly the
case neither earlier sample could contain.

**And it still does not clip.** The rail is only reached on the *unity*
pass, which is the first play, while measuring -- and 0 dBFS is full
scale, not past it. Every play after that has a sidecar, and the track
above measures -6.04 LUFS at -0.07 dBFS peak and gets **-11.96 dB**:
its output peak came out at 7824, or -12.44 dBFS, which is the gain
applied exactly as designed.

So the entry closes as **reached, never exceeded**. A limiter would have
had nothing to do on either pass: at unity there is no gain to overshoot
with, and once a gain exists it is negative and large. What made this
answerable was one log line added instead of a feature.

### The cover cache is ninety times its documented size (1006)

`mediacache.h` states the budget:

    cover, compressed    80-120 KB stored

The board, on an album whose cover is a 3.7 MB PNG:

    prefetch done: cache 3 entries, 10935 KB

**Three slots, three copies of the same 3.7 MB image, 10.9 MB of
PSRAM** -- next to the 1.8 MB shadow buffer and a decode ring with audio
running through it. Nothing is leaking and nothing is wrong with the
cache: `COVERTAG_MAX_IMAGE` is 4 MB, three slots of that is 12 MB, and
the design permits every byte of it. **The stated figure was an
assumption about cover art, not a bound on anything.**

**It has a measured cost, and it is the first real contention the
arbiter has ever seen.** The same track change:

    track playback: 20 reads, 5119 KB ... worst hold 387 ms, worst wait 234 ms
    track prefetch: 240 reads, 3644 KB in 326 ms held

Every earlier log in this file reports `worst wait 0 ms` or 1 ms. The
arbiter worked -- it broke the prefetch into chunks and let playback in
-- but `worst hold 387 ms` is the floor on control latency by the
argument in "Bytes over wall-clock is not a throughput", so a button
pressed behind that read waits a third of a second. The prediction in
"The card is arbitrated, not throttled" was that PLAYBACK's worst wait
should be roughly one chunk of the current device. 234 ms is not that.

Deliberately **not** fixed here, because every available fix is a
behaviour change and they are not equivalent:

- **Cap the cached size** and large covers stop being cached at all,
  which costs the prefetch on exactly the albums where the decode is
  most expensive.
- **Downscale before caching** and the cache stops holding what the file
  contained, which is a different thing from what every other consumer
  of `covertag_extract_art()` gets.
- **Do not prefetch covers past some size**, which keeps the cache
  honest and gives up the head start.

That is a decision about what the player should do, not a defect to
correct, and it wants taking on its own rather than inside a patch that
was closing something else. **What is fixed here is the documentation,
which claimed a number the code never enforced.**

Applied in `player.c`'s decode loop, not `audio_out.c`, because only the
USB route has a software gain stage -- the analog path writes ES8388
registers, so a gain applied there would play the same track at two
levels depending on what is plugged in.

**Measuring and applying are mutually exclusive**, and have to be: a pass
that applied a gain would measure the gain back and converge on the
reference whatever the track is. The sidecar's presence is the switch.

On one real album: nine tracks spanning -17.36 to -25.13 LUFS, a 7.77 dB
spread, collapse to 0.18 dB. The residual is two tracks the peak held
back.

### What a play costs, and what it does not

The first uninterrupted play of a track produces its loudness and its
envelope. **Seek, skip, or next during that play throws the measurement
away** -- BS.1770 integrates over the whole programme, so a measurement
that skipped a section is not a slightly worse number, it is a number
about different audio, and nothing downstream could tell. A restart is
not a special case: it re-enters `play_file()`, which resets the
accumulator, and that attempt writes if it reaches the end.

A track always skipped through therefore gets neither, for ever. That is
counted (`attempts.abandoned`) so a future policy can stop trying or
accept a partial answer; nothing acts on it yet.

## The sidecar

`.<name>.rgcache` next to the track -- a dotfile, which
`storage_is_hidden()` already excludes from every listing and playlist
scan, so nothing had to learn to hide it. JSON Lines, though in practice
**always exactly one line**.

Keyed on **size and mtime, not a content hash**. A hash would need to
read the file to validate a cache that exists to avoid reading the file.

### One line, rewritten whole

Every write serialises the entire merged record to a temp file and
renames it over the old one. Appending was tried and was wrong, and the
way it failed is instructive: a card showed a sidecar with six lines
across three format versions, four of them dead, because the "is this
even our format" check only asked whether the first byte was `{` -- and a
stale-version line is perfectly good JSONL. Every format bump would have
doubled the corpses.

The deeper point is that appending never bought anything. Every line is
already a complete merged record, because the caller loads and overlays
before writing: line N+1 says everything line N said. An append writes
exactly the bytes a rewrite would **and** keeps the old copy. The only
argument for it was torn-write safety, and temp-and-rename answers that
better, because the rename either happened or it did not.

This is also the fragmentation answer. At about a kilobyte -- 5.4 KB with
a full 256-entry seek index -- the file sits inside a single 64 KB FAT
cluster and cannot fragment, because it never grows. There is no size
threshold: nothing to compact and no boundary to straddle. A growth cap
of 65536 was in fact the worst possible value, permitting growth to
exactly the cluster edge before acting.

### Two version numbers, deliberately

`REPLAYGAIN_FORMAT_VERSION` is the record's shape; `LOUDNESS_VERSION` is
what the numbers mean. They change for different reasons and must not
invalidate each other: adding a field should not throw away a good
measurement, and changing the gate or the weighting should not throw away
an envelope. A stale loudness version reports absent and is recomputed on
the next full play; the rest of the record survives.

Format version 2 exists because version 1 stored the frame walk's proxy
envelope under the same key -- same shape, different meaning, and an
unbumped reader would have drawn old numbers as amplitude and been wrong
invisibly.

### Absent is not the same as none

The art section carries `present` **and** `has_art`. Section absent means
nobody has looked; section present with `has_art` false means somebody
looked and there is none. The negative is the valuable half: it turns the
eight-to-thirteen second tag scan the logs used to show into nothing at
all.

The positive is deliberately **not** used to seed the cover on the open
path. The sidecar stores where the image is, not the image, so using it
means a seek and a read -- that belongs where the decode already happens,
not where it would block the open.

### Held for the track, written once

`play_file()` reads the record once, before `decoder_open()`, and merges
into it in memory as facts are learned -- tags, whether there is a cover,
the format the decoder reports, then the loudness and envelope at the
end. One write when the track ends, and only if something changed.

That last clause is what makes a fully-known track free: it reads its
sidecar once and never writes. The format merge compares before marking
dirty, or a track whose format was already recorded would rewrite an
identical file every play and the dirty flag would be decoration.

Written outside the `TRACK_ENDED` test, because a skipped track still
learned its tags and whether it has a cover even though its loudness was
thrown away.

The read has to happen in `track_change_begin()` rather than
`play_file()`, and this was got wrong once: seeded one step downstream of
its consumers, `load_tags()` re-read the ID3 and `do_art()` re-scanned
for a cover the sidecar already said was absent, and both then wrote back
what was already there.

### What is in it, and what is not

Waveform, loudness, tags, format (rate, channels, bitrate, codec,
duration, gapless delay/padding), art location, abandonment count -- all
produced and consumed.

**The seek index is wired as of 0703.** It was the largest remaining
win and it was the last: with everything else answered from the sidecar
before `decoder_open()` returns, `index built` was essentially the whole
of the 1.2-1.8 s open. See "The seek table is harvested, not scanned
for".

Entries are (offset, sample) pairs at a **stored** spacing -- 10 s by
default, not minimp3's per-frame, because one pixel of a 720 px bar is
380 ms on a 273 s track and per-frame precision is far finer than a
finger can ask for. A long file doubles the spacing to stay under 256
entries, so a reader must use the stored value and not the constant.

Two read-side rejections, both silent-wrong-seek hazards rather than
crashes: ragged offset/sample arrays (pairing an offset with the wrong
sample seeks to the wrong place), and any offset past the end of the file
(a record about a different file that happened to match size and mtime).

### The seek table is harvested, not scanned for (0703)

An MP3 with no Xing header states nothing about its own length, so
minimp3 finds out by walking every frame header in the file --
`MP3D_SEEK_TO_SAMPLE` at open. That walk is where the duration and the
seekability come from, and it is 1.2 to 1.8 seconds of every play in
every log taken since 0105.

**It is a whole-file read whose result is the same every time.** The
sidecar has held a place for that result since 0200 -- `(offset,
sample)` pairs at a stored spacing -- and 0703 fills it and reads it
back:

| | Xing-less MP3 |
| --- | --- |
| First play | scans as before, and the table is harvested from what the scan built |
| Every play after | `MP3D_DO_NOT_SCAN`, table installed, open reads one frame |

Nothing is walked to produce the table. `decoder_index_extract()`
decimates the index minimp3 has already built, which is the same shape
as the loudness measurement and the envelope: the expensive step is one
the player was having anyway, and this is arithmetic on a structure that
already exists.

**A Xing-tagged MP3 gets one too, from the other end.** It never scans
at open -- minimp3 stops as soon as it finds the tag -- and pays instead
on the first drag, inside `mp3dec_ex_seek()`, which builds the index
lazily. Same walk, moved to a worse moment. Harvesting is at the end of
the track either way, so whichever walk happened is the one that gets
recorded.

Details that are load-bearing:

- **The record counts PCM frames; minimp3 counts int16 values across
  all channels.** The multiply is in `minimp3_install_index()` and the
  divide in the extract, and getting either wrong seeks to half or
  double the requested point on stereo -- the same trap `ex.samples`
  sets two functions away, and the reason the stored format is the
  codec-neutral one.
- **`indexes_built` is the claim, not a poke at internals.** It is the
  flag minimp3 sets when its own scan has completed, and installing a
  table asserts exactly what that flag asserts. `mp3dec_ex_close()`
  frees `index.frames` unconditionally, so the allocation is handed
  over rather than owned by `decoder.c`.
- **A table that fails validation is ignored, not half-installed.**
  Out-of-order pairs seek to the wrong place and nothing downstream can
  tell, which makes them worse than no table at all; a rejected one
  leaves `indexes_built` at 0 and minimp3 builds its own on the first
  seek. Slower than intended and never wrong. `replaygain.c` already
  rejected ragged arrays and offsets past the end of the file for the
  same reason; this adds monotonicity, which it could not check without
  knowing what the pairs mean.
- **`MP3D_DO_NOT_SCAN` is used only when there is a table.** Without
  one the scan is still the only source of a duration for a Xing-less
  file, and switching it off unconditionally trades a slow open for a
  dead seek bar. That is what `BOUNDARY_NO_INDEX` does deliberately and
  temporarily, and it is not a default.
- **The harvest is outside the `TRACK_ENDED` test.** An index is a fact
  about where the frames are, not a measurement of the audio, so a
  skipped track has learned it as completely as one played through --
  the same reasoning the tags and the art flag are written under, and
  the opposite of the loudness, which a skip invalidates.

**The cost, stated rather than discovered.** Entries are ten seconds
apart and minimp3's own are 26 ms apart, and `mp3dec_ex_seek()` backs
off `MINIMP3_PREDECODE_FRAMES` *entries* before the target to fill the
bit reservoir -- two frames' worth on its own index, twenty seconds'
worth on this one -- then decodes forward to the sample asked for. So a
seek can cost up to thirty seconds of MP3 decode where it used to cost
a lookup, and the decode loop cannot look at a button while it is in
there.

That is the trade: a few hundred milliseconds on each seek against 1.2
to 1.8 seconds on every play. It is worth taking and it is worth
measuring, which is why `decoder_seek_sec()` now logs anything over
100 ms and says whether a table was in use. **If that number is bad,
the answer is a denser table, not a return to scanning at open** -- the
spacing is stored in the record precisely so it can change without
invalidating what is already written. The prediction to falsify: a
44.1 kHz stereo track should seek in well under 500 ms with a table,
and the same line should read as the whole file on the first drag of a
Xing-tagged track that has never been seeked in.

### The listening text

A track with no envelope yet draws an unshaped grey bar with
"ReplayGain is listening..." across it. Grey and unshaped rather than a
red progress fill, because a red fill exactly where the waveform will go
reads as a waveform of a uniformly loud track rather than as one not
measured yet.

Gated on whether a measurement is actually running, **not** on the
envelope being absent -- those are different states. A track whose
envelope is already in its sidecar is not being listened to, and the gap
between the track starting and the bar being handed that envelope was
otherwise putting the words on screen for a few seconds of every replay,
claiming work that was not happening on exactly the tracks that had
already done it.

## USB audio output, and why it wins

`uac.c` is the USB Audio Class output and `audio_out.c` decides when it
plays. The rule is one line: **a USB audio device that can take the format
wins.** It outranks the headphone jack, which outranks the speaker.

Unconditional rather than a preference, for the same reason the jack has
always beaten the speaker without asking: plugging a DAC or a headset into
a player is not an ambiguous act. This adds a rung above the existing rule
rather than inventing a new kind of rule.

### "Can take the format" is doing real work

There is no resampler. A device that only offers 48 kHz is not an output
for a 44.1 kHz file, and the correct fallback is the analog path -- not
handing over the bytes anyway, which is a semitone flat and 9% fast and
reads as a broken player rather than as an unsupported device.

So the decision is per format and re-made on every track. An album of
44.1 kHz files with one 48 kHz track in it routes to USB, drops to the
speaker for that track, and goes back. That is visible in the log and it
is not a bug.

This is also the one inversion from the UAC example this was ported from.
`uac_example.c` picked the first 16-bit PCM alternate and took its first
listed rate, which is right when you are looping a microphone into a
speaker and only need the two ends to agree with each other. A player
already has a rate -- the file's -- so the search runs the other way:
state a rate and a channel count, get an alternate that offers exactly
that, or get `ESP_ERR_NOT_SUPPORTED`.

### Muting the amplifier is not enough

With headphones in, `SPK_EN` is already low. Cutting only the amp when USB
takes over therefore leaves the ES8388 driving OUT1, and the 3.5 mm jack
plays the same track as the USB headset a few milliseconds behind it.

So the DAC is muted as well -- `DACCONTROL3` bit 2, one write, both output
pairs at once.

**The I2S channel stays running, and stays at the right rate.** Stopping
it drops MCLK, and the ES8388 stops answering on I2C without MCLK, so
coming back would be a codec re-init rather than a register write. The
rate is set even on tracks that route to USB, so a device unplugged
mid-track falls back in one block instead of having to reconfigure a clock
with audio in flight. The cost is a clock running into a muted DAC, which
is the state the part is in between tracks anyway.

**The jack's poll task no longer drives `SPK_EN` directly.** It is one
input to `arbitrate()` now. Unplugging headphones while a USB device is
playing must not switch the speaker on underneath it.

### The device handle is the exception to the publish-a-value rule

This file says, at length, never to share a handle across tasks. `uac.c`
has to: writing audio means calling the driver with the handle, and the
writer is not the task that opens or closes it.

So `s_dev` is under a mutex, and the disconnect callback **does not
close**. It publishes `s_present = false` and queues; the event task does
the close with the lock held. Closing a device while a writer sits inside
`uac_host_device_write()` on it is precisely the class of bug the heap
corruption section is about, and this one would be a genuine
use-after-free rather than a stray read.

A disconnect therefore costs the length of one in-flight write, bounded by
the caller's timeout. `uac_present()` reads the published bool and never
takes the lock, so the UI cannot block behind a write in flight.

### A stalled device drops the block

`uac_write()` waits up to 200 ms for room. A USB frame is 1 ms and the
driver ring holds about 93 ms, so a healthy stream never comes near it --
it is a stall detector, not flow control. Past it the block is dropped and
logged rather than retried, because the writer task is what the transport
buttons are queued behind and a wedged device must not become a dead play
button.

### The volume slider works either way

Asked of the device once per route change, not per track, and the failure
is latched. Most of the cheap class-compliant parts -- the C-Media ones in
particular -- have no feature unit the driver can reach, and a volume drag
emits one request per poll; probing fifty times a second to learn the same
no is both noisy and slow.

When there is no device control, gain is applied to the samples on the way
out, into a scratch buffer rather than in place, because the block belongs
to the caller's ring. The curve is linear in amplitude, which is the wrong
curve for a volume control and is deliberately the *same* wrong curve
`es8388_set_volume()` uses: the slider should not feel different depending
on what is plugged in. If that is ever fixed, both change together.

### The microphone is ignored

`UAC_HOST_DRIVER_EVENT_RX_CONNECTED` is logged and nothing is opened.
Nothing in a music player reads audio in, and an open RX interface costs a
ring buffer and isochronous bandwidth for a stream that would only be
discarded. The example opened it because it was looping mic to speaker.

Note that a headset is **two logical UAC devices**, one Audio Streaming
interface each, and the driver's connect callback fires per interface
rather than per device. A second TX interface is left closed rather than
arbitrated: there is one pair of ears and no way to ask which.

### The stream is not started at attach

There is no format to start it in until a track is playing, and a stream
running with nothing written to it is isochronous bandwidth spent on
silence.

### What this does not do yet

- ~~**Nothing on screen says which output is playing.**~~ Fixed in 0901.
  The guess in this entry was right about the place and wrong about the
  method: no new indicator was needed, because the icon already next to
  the volume slider was a picture of the output that only happened to be
  correct one third of the time. See "The speaker icon was lying" below.
- **No resampling**, per above, so a 44.1 kHz-only device and a 48 kHz
  file fall back to the speaker rather than converting.
- **UAC 2.0 is untested.** The driver claims it; the device this was
  written against is a UAC 1.0 C-Media part.
- **Bus power is still USB 2.0.** A bus-powered DAC that wants more than
  the port will give brown-outs rather than failing to enumerate, which is
  the same caveat the mass-storage note already carries.

### Joining paths is not a snprintf

`storage_join_path()` exists because the obvious version does not build:

    snprintf(out, sizeof(out), "%s%s%s", dir, sep, name);

`dir` and `out` are both 512 bytes, so the concatenation cannot be proven
to fit and `-Wformat-truncation` says so -- correctly, and as an error
under the project's warning settings. Silencing it would have been the
wrong call anyway: a truncated path is a path to a different file, or to
none, and quietly opening the wrong one is worse than not opening it.

So the join is written out, the arithmetic is the proof, and it returns
false rather than truncating. Every caller checks. The chooser logs and
ignores the row; the playlist scan skips the entry and carries on, because
one unreasonably long filename should not cost you the rest of the album.

It lives in `storage.c` rather than in each caller for the usual reason --
`browser.c` and `playlist.c` both build paths the same way, and the second
copy is the one that drifts.

### Unmounting under an open file

Removal is detected by polling, so the interesting case is a card pulled
mid-track. Calling `esp_vfs_fat_sdcard_unmount()` with a `FILE*` still
open on the volume is a use-after-free inside FatFs rather than an error
return.

So the two halves are split. `storage_hold()` marks the volume the decoder
is reading; on removal that volume is flagged absent immediately -- the
tab greys, and the decode loop sees its own volume vanish and stops the
track -- but the unmount itself waits for the release. One poll later,
with the decoder closed, the unmount happens for real.

The playlist is cleared at the same time. Keeping it would offer a next
track whose path is on a volume that is no longer there.

### The chooser

`browser.c`, full screen rather than a panel over the artwork. A chooser
that respected the cover would get a handful of rows in what is left and
need scrolling several times as often -- and the artwork is not
information while you are picking something else to play.

It owns no task. `ui_task` drives it -- `browser_touch()` then
`browser_draw()` -- exactly as it drives the transport bar, so there is
one writer to the framebuffer and no lock. That is also why the chooser is
*opened* from the UI task and only *requested* from the decode loop.

A tab for a volume that is not there is drawn greyed, not hidden. A tab
that disappears when the card is out and reappears when it goes in moves
the other tab under the finger.

Redraws are gated on a dirty flag plus `storage_generation()`. The flag
covers taps; the counter covers a drive appearing while the chooser is
already up, which has to be visible without a touch. Without the gate this
is a full 720x1280 blit ten times a second against a decoder that wants
the same PSRAM bandwidth.

Files the decoder cannot open are hidden rather than greyed. A card root
is mostly `System Volume Information` and stray text files, and a list
where two thirds of the rows are untappable is a worse list.

Folders sort before files, each run case-insensitively. Mixing them
alphabetically buries a disc subfolder in the middle of the track list,
and the two are different kinds of thing to tap.

Scrolling is two page buttons, not a flick. Flick physics needs velocity
tracking across a poll interval that changes from 20 ms to 100 ms
depending on whether a finger is down, and the scroll bar down the right
edge already says where you are.

### Folders are the playlist

`playlist.c` holds one directory's worth of playable files. A folder is
the unit because a folder is what an album is on disk, and nothing is
persisted -- the list is rebuilt from the directory each time one is
chosen, so a file added on a desktop appears the next time that folder is
opened rather than after a rescan nobody remembers to run.

Tapping a track loads its folder as the list and starts there, so "play
this one" and "then carry on" are one choice rather than two. `FLDR` plays
the current folder from the top.

The scan is not recursive. An album with disc subfolders is two choices
rather than one, which is the honest rendering of what is on the card; a
recursive scan of a card root is a several-thousand-entry list and a long
stall on the touch that asked for it. `PLAYLIST_MAX` and `MAX_ENTRIES` cap
both lists for the same reason -- each entry is a `strdup` on a touch
event.

Sorting is not optional. FatFs hands entries back in directory order,
which is creation order on most cards, so an album copied track by track
is roughly right and an album copied by anything that parallelises is not.

`ONE` / `ALL` / `RND` cycles in the chooser's footer. Shuffle keeps a
played-bitmap rather than picking uniformly at random, so a twelve-track
album plays twelve different tracks; the bitmap clears when it fills,
minus the track just played, so the wrap is a fresh shuffle rather than a
repeat and never doubles a track across the seam.

### One track after another

`play_file()` returns why it stopped -- ended, interrupted, media gone --
because the caller has to tell "the file finished, go on to the next" from
"something else was chosen, do not".

Three things that were free with a single file and are not any more:

- **The PCM ring is per track now.** It used to be created once and never
  freed, which was correct when the function ran once. Freeing it while
  `i2s_writer_task` is blocked inside `xStreamBufferReceive()` on it is a
  use-after-free once per track, so the writer sets a flag on its way out
  and `play_file()` waits for it.
- **An interrupted track drops what is queued** rather than draining it.
  0.37 s of the old song after the tap sounds like the tap was ignored --
  the same reasoning as the seek path. A track that ended on its own still
  drains, because those fractions of a second are the end of the song.
  The drop goes through `s_pcm_flush` and the writer; this site used to
  reset the ring directly and its own comment named the hazard ("the
  writer may be parked on this ring") while treating it as safe.
- **The cover is cleared between tracks.** `albumart_show()` draws but
  never clears, so a track with no art inherited the previous track's
  cover, which reads as the player having ignored the choice rather than
  as the file having no picture in it.

`s_path` became a static for the same reason: `s_display_name` points into
it and the UI task reads that every frame, so a local would have gone out
of scope the moment the second track started.

The chooser draws over the artwork, so closing it has to repaint. That
happens on the decode loop rather than the UI task, because it `fopen()`s
the track and pushes a JPEG through the hardware codec, and the UI task
has a 20 ms period. It is checked at the top of the decode iteration and
again in the idle path, or a chooser dismissed while paused -- or with
nothing playing at all -- leaves its listing on screen.

### Pause stops the writer, not the decoder

It used to stop the decoder. That was right when the ring held 0.37 s:
stall the producer and the consumer empties in a third of a second. Once
the ring held tens of seconds -- 59 s at the size it was when this was
written, about 20 s at today's 3520 KB -- stalling the producer left the
writer to play all of it out -- pause fell silent up to a minute after
the press, with `audio_out_set_idle()` cutting the amp somewhere in the
middle, so the symptom read as the player starting up again on its own
rather than as a late pause. The ring grew 160x and this was not
revisited.

Worth stating rather than just fixing: **pause against a buffer is only
immediate at the end the listener hears, and which end that is does not
depend on the size.** Whatever `PCM_RING_BYTES` becomes, the gate belongs
in `i2s_writer_task()`.

The ring is deliberately not drained. Its contents are still the correct
next samples, so resume is instant and costs the card nothing -- which on
a card that stalls `decoder_read()` for five seconds at a time is the
difference between resuming and resuming into the next stall. The decode
loop carries on filling and then blocks in `xStreamBufferSend()`, which
is where it blocks during ordinary playback anyway.

`s_writer_stop` exists because teardown ends with two spins -- drain the
ring on `TRACK_ENDED`, then wait for `s_writer_done` -- and both wait on
the writer to move. A writer parked on a pause never does, so pausing at
the wrong moment would hang the decode loop against a task that is
deliberately not running, taking the seek and next-track paths with it.
Being paused during teardown means the tail of a finished track plays
out. Deadlocking means the player stops answering. It is set before the
waits and cleared with the other two flags when the next ring is made.

### Nothing to play is a screen, not an exit

`app_main()` used to give up and return when the card had no playable file
in its root, leaving a lit panel attached to a dead task. It now opens the
chooser instead, with both tabs greyed if that is the truth, which is at
least a place to plug something in.

Autostart is still the first playable file in the root of the first
mounted volume, and its folder becomes the list -- a card with an album on
it plays the album without anyone choosing anything. A card whose root is
nothing but folders opens the chooser, which is the honest answer to "what
should I play" when there is no file to pick.

### One shadow buffer, and the screen stops flashing

Everything is drawn into a PSRAM shadow and copied to the panel a band at
a time. It used to be written straight into the buffer the DPI peripheral
scans out of, which meant every intermediate state was displayed:

- the bar cleared to grey a moment before its contents arrived, on every
  repaint, and
- far worse, `albumart.c` `memset` the **whole panel** to black and then
  filled it back in over the length of a PNG decode. A track change was a
  full-screen black flash by construction.

The old code's comment defended writing the scan buffer in place, on the
grounds that `draw_bitmap` only writes back the cache for the rectangle it
is given. That is true and it is not the problem -- the problem is that
the pixels are live between the `memset` and the last `memcpy`, and the
panel is reading them the whole time.

Two things follow from the shadow being separate:

- **Every blit is full width.** A full-width band is contiguous in both
  buffers, so it is one `memcpy` and one `draw_bitmap`. A sub-width
  rectangle would need a row loop and a stride the driver does not take.
- **`albumart_show()` is given the artwork height, not the panel height.**
  It clears what it is given, so passing the full panel would blank the
  transport bar in the shadow and blit that over it -- the bar would
  disappear on every track change until the next `ui_draw()` put it back.
  The caller passes `UI_ART_H`.

The cost is 1.8 MB of PSRAM and a copy per redraw. The copy is of the band
actually redrawn, which for the transport bar is `UI_BAR_H` rows rather
than 1280.

### The cover is decoded in whole MCUs

The P4's hardware JPEG decoder works a macroblock at a time, so it writes
a picture rounded **up** to the MCU grid, padding included. Two things
follow, and `albumart_draw()` had both wrong.

**The output buffer has to hold the padded picture.** A 3000x3000 cover at
4:2:0 is 3008x3008, which is 18,096,128 bytes rather than the 18,000,000
an unpadded `width * height * 2` asks for, and the driver refuses the
decode over the 96 KB difference:

```
E jpeg.decoder: Given buffer size 18000000 is smaller than actual jpeg
                decode output size 18096128
E tab5_art: albumart_draw(286): jpeg decode
W tab5_mp3: cover art failed to decode (ESP_ERR_INVALID_ARG)
```

**The padded width is the row stride.** Copying out at `info.width` shifts
every row relative to the one above it -- a picture sheared diagonally
across the screen. That one was latent rather than absent: it needs a
cover whose width is not already a multiple of the MCU, which 500 and 1000
both are not, so it was there from the first version and the log had
nothing to say about it, because the decode had succeeded. The `out_size`
the driver reports is now cross-checked against the computed padded size,
so a disagreement is an error rather than a shear.

MCU size follows the chroma subsampling -- 16x16 at 4:2:0, 16x8 at 4:2:2,
8x8 at 4:4:4 and greyscale -- so it is read from `info.sample_method`
rather than assumed to be 16.

**Oversized covers are scaled to fit at a fractional ratio.** Two
revisions, and the second is the interesting one.

Cropping alone was the original: a 720 px square cut from the middle of a
3000 px cover, under a quarter of the picture, with nothing on screen or
in the log to say so. Then an integer decimation -- every Nth pixel, N the
largest that still covered the panel -- which fixed the 3000 px case and
left the common ones badly served. 1920 over 720 is 2.67, so N was 2, the
cover came out at 960 px, and a quarter of it was *still* cropped away. An
integer step can only land on the panel exactly when the cover is a
multiple of it, and covers are round numbers of their own rather than
multiples of a panel.

So the step is 16.16 fixed point. Same shape and same cost -- one shift
and one multiply per output pixel, no second buffer -- and the picture
lands on the panel exactly. 1920 becomes 720 whole rather than 960
cropped.

Fit rather than fill, so nothing is lost. A cover that is not square gets
black at two edges instead of having its other two trimmed. The cover is
the thing being shown, and a player that quietly crops the artwork it was
given is deciding something it was not asked to decide.

Nearest neighbour, no filtering. A box filter would be visibly better on
fine detail and would read every source pixel rather than one in seven,
during playback. Album art is not fine detail.

**The allocation is checked before it is attempted.** Full size or not at
all means a 3000 px cover wants 18 MB of PSRAM for a 720 px square, on a
board also holding the 1.8 MB shadow buffer, the bitstream and a decode
ring with audio running through it. It usually fits; when it does not, the
useful thing to print is how much was wanted, in one line, rather than
whatever the driver says on the way down.

**And 1010 asks the second question, which had never been asked.** The
board found a cover that does not fit:

    cover needs 17672 KB of PSRAM in one block, largest free is 16128 KB
    cover art failed to decode (ESP_ERR_NO_MEM)

`largest` says the allocation cannot be made now. It does not say whether
it could ever be made, and those want different fixes. If the **total**
free is also short, nothing can be reclaimed into it and the only answers
are a smaller decode or none. If the total is comfortably over while the
largest is not, the PSRAM is fragmented and a tenant might be worth
evicting -- the cover cache is the obvious candidate, since the same log
shows it holding 5483 KB of compressed images, and the largest block fell
from 16128 KB to 14080 KB as it filled.

**"Free the cache and retry" is worthless in the first case and worth
writing in the second, and no log so far tells them apart.** So 1010
prints the total beside the largest and names which case it is, and
`do_art()` adds what the cache was holding at that moment -- `albumart.c`
reports the PSRAM picture and cannot see the cache, and the two together
are what say whether it is the tenant to reclaim or a bystander.

This is instrumentation before the patch, which is the thing the cyan
flash cost six patches for skipping and which 1003 got right by
accident of being asked to build a limiter. **A reclaim-and-retry is
also not safe to write blind**: `do_art()` may be holding a *borrowed*
pointer into the cache it would be clearing, and `mediacache_clear()`
would free the image being decoded out from under it. That is a second
reason to measure first rather than to write the plausible fix.

### A 64-bit divide per pixel is a watchdog reset (1005)

A 1600x1600 PNG cover -- Thumper's, 3.7 MB -- took the media task down
with it:

```
E task_wdt: Task watchdog got triggered.
E task_wdt:  - IDLE1 (CPU 1)
E task_wdt: CPU 1: media
MEPC : 0x4fc14d32   --- __moddi3 in ROM
RA   : 0x40016138   --- png_on_draw at main/albumart.c:825
```

**`__moddi3` is the giveaway.** The P4 is RV32: it has a hardware 32-bit
divider and **no 64-bit one**, so every `int64_t` division becomes a call
into a software routine in ROM. `png_on_draw()` had four of them:

    const int px0 = c->dx + (int)(((int64_t)x * c->cw) / c->iw);
    ... and three more for px1, py0, py1

pngle is a streaming decoder and calls the draw callback **once per
pixel** for a non-interlaced image -- `w` and `h` are 1. So a 1600x1600
cover is 2,560,000 callbacks and **10,240,000 software 64-bit
divisions**, on the media task, with nothing yielding. The watchdog
fired 5.4 s after `cover is 1600x1600 (png)`.

**The 64 bits were never needed.** The largest product formed is
`iw * cw`, and `cw` is bounded by the panel at 720. Overflowing a signed
32-bit int needs a source image about 2.98 million pixels wide. The
casts are 32-bit now, which is one hardware instruction.

**Checked as equivalence, not as a rewrite.** The old and new
expressions were run against each other over 196 source geometries from
1 px to 100000 px, at every source column: 185,948 edge computations,
zero mismatches and zero products exceeding `INT32_MAX`. The scaler
draws exactly what it drew before, which matters because the edge
arithmetic is what makes adjacent runs abut -- see the section below.

`PNG_MAX_DIM` (100000) is the bound that keeps that proof true, checked
in `png_on_init()`. **Refused rather than clamped**: clamping would draw
a silently wrong picture, and the failure returns
`ESP_ERR_NOT_SUPPORTED` so the format card appears instead of a black
panel.

**Why this survived until now.** The JPEG path has always used a 16.16
fixed-point step and never did any of this; only PNG covers take the
edge path, and only a *large* PNG makes it expensive. Small PNG covers
had been fine for months. The first 1600x1600 one crashed the player.

The general form is worth keeping, because this file has met the shape
before from the other side: **an int64 cast written to be safe is not
free on a 32-bit target.** 0709's MP4 reader was a pattern a desktop
buffer hid and the board counted; this is a pattern a desktop *CPU*
hides and the board counts. Both looked correct in review and cost
seconds on hardware.

#### Every other 64-bit divide was surveyed, and none of them move (1007)

1005 was found by a file rather than by looking, so 1007 looked. Every
64-bit division and modulo in `main/` was classified by how often it
runs and whether the range needs the width. **Nothing else is being
changed**, and the more useful half of the result is the list of places
where 64 bits are load-bearing -- so that a later patch reading "remove
the int64s" as the lesson of 1005 does not break large-file support to
save cycles nobody is spending.

**Hot -- per sample or per pixel:**

| Site | Rate | Verdict |
| --- | --- | --- |
| `albumart.c` `png_on_draw()` | 10.2 M per 1600px cover | fixed in 1005; proved narrowable |
| `player.c` `fade_apply()` | 44100/s, only during a fade | **64-bit required** |
| `player.c` `xfade_mix()` | 2 per chunk | already hoisted, deliberately |

`fade_apply()` is the only remaining per-sample 64-bit divide and it
**cannot** be narrowed. `FADE_OUT_MS` is 3000, so at 44.1 kHz the ramp
is 132300 frames and `32768 * 132300` is 4,335,206,400 -- past
`UINT32_MAX` by 40 million. At 48 kHz it is worse. Narrowing it would
overflow in the middle of the one code path whose whole job is to reach
silence smoothly, and the symptom would be a fade that jumps to full
volume near its end.

It is also not worth restructuring. It runs at the sample rate for three
seconds, which is about 3% of one core for the length of a fade, against
1005's ten million in a burst with nothing yielding. **The two are the
same construct four orders of magnitude apart**, which is the actual
lesson: the width was never the problem, the multiplication by how often
it runs was.

`xfade_mix()` already got this right on its own and says so -- it takes
its two divides per chunk and interpolates, "so the mix loop stays two
multiplies and an add per sample". It is the pattern `png_on_draw()`
should have used and the reason the crossfade never showed this fault.

**Cold -- once per track, per image, per table entry or per draw:**
`replaygain.c`'s envelope merge (bounded by columns), `decoder.c`'s
table conversion (256 entries), `albumart.c`'s fit and 16.16 step (once
per image), `cbrseek.c`'s sample points (five), `loudness.c`'s envelope
span (once per open), `ui.c`'s drag target (25 Hz), and the various
rate-to-frames conversions in `player.c`. None of these runs often
enough to measure.

**Load-bearing, and not to be narrowed under any circumstances:**

- **`mp4seek.c`'s `be64()` and the `co64` path.** These are 64-bit
  chunk offsets, which is the box a muxer writes precisely when a file
  is past 4 GB. Narrowing it breaks exactly the large files exFAT is
  enabled for. 0717 already settled this in the same words -- the
  `<= UINT32_MAX` guard there is against a genuine `uint64_t` and *is*
  a check, unlike the one it found in the recording path. **Type, not
  habit.**
- **`storage.c`'s capacity.** `csd.capacity * sector_size` overflows 32
  bits on any volume past 4 GB, which is every card this player is
  likely to meet -- the test rig's is 8 GB and reports correctly only
  because of this.
- **`oggseek.c`'s granule positions**, which are 64-bit in the Ogg
  container by specification, and `tsseek.c`'s 33-bit PTS.
- **`player.c`'s `frames_out`**, which counts output frames for the
  length of a track and is the anchor for the elapsed clock.

The general rule, stated so it survives the next optimisation pass:
**64-bit width in this program is either a range requirement or a hot
loop, and it is worth checking which before touching either.** 1005 was
the second kind and was worth a patch. Everything else here is the
first, and narrowing any of it would trade a correct player for cycles
that were never being spent.

### The JPEG decoder's `rgb_order` is a byte scramble, not a colour order

A gold cover on a deep red background rendered as silver on blue. Not a
format problem and not the decode -- red and blue exchanged.

`jpeg_decode_cfg_t.rgb_order` does not name the output colour order. It
picks a DMA2D **byte** scramble applied after the colour conversion, and
for RGB565 output the two settings come out as:

| Setting | Scramble |
| --- | --- |
| `JPEG_DEC_RGB_ELEMENT_ORDER_RGB` | `DMA2D_SCRAMBLE_ORDER_BYTE2_0_1` |
| `JPEG_DEC_RGB_ELEMENT_ORDER_BGR` | `DMA2D_SCRAMBLE_ORDER_BYTE2_1_0` (identity) |

A 16-bit RGB565 word in little-endian memory does not survive having its
bytes reordered. So the setting whose name matches the panel is the one
that corrupts, and `..._ORDER_BGR` is what produces a native RGB565 word.

`(200,150,50)` read back as `(50,150,200)` is gold to blue exactly, and
the near-grey highlights on the emblem were unchanged because swapping R
and B does nothing to a pixel where they are equal -- which is why it read
as *silver and blue* rather than as a uniform colour shift.

The rest of the file is the control. `gfx.c`'s `RGB()` macro, the pngle
path's hand-packed pixels and the DPI panel's own `LCD_COLOR_FMT_RGB565`
all agree with each other; only the JPEG path disagreed, and only for
JPEG covers. A PNG cover has always been right.

`conv_std` is now stated rather than left at 0. It happens to be BT.601
either way, which is the right answer for JPEG, but a colour standard
arrived at by zero-initialisation is not a decision.

### Cover art has its own task

`load_track_visuals()` runs on the decode loop, and it used to read the
cover out of the tag and decode it there. The ring was 64 KB at the time
-- 0.37 s of 44.1 kHz stereo, against today's 20 s -- and a 3000x3000
cover took 550 ms in the hardware
decoder alone, after a 511 KB read off a USB drive. Every large cover was
therefore spending longer than the ring holds, before anything progressive
or software-decoded enters the picture.

So the read and the decode moved off it, and what stays on the decode loop
is tag parsing and one flag. Same compute-and-hand-off shape as the frame
walk -- and, in fact, the same task.

**Both slow per-track jobs share one background task, in order.** They
started as two, and on a USB drive that was the wrong shape: both open the
same file on the same slow device at the same moment, the cover reading
half a megabyte out of the tag and the walk reading the whole file, so
they spent the first seconds of every track taking turns at the same queue
and finished later than either would have alone. One at a time is not
slower; it is the same total read with the contention removed.

Art first, and not because it is smaller. The cover is the largest thing
on screen and a track change blanks it, so the seconds before it arrives
are the ones that read as the player having stalled. The envelope arriving
late is invisible -- the bar falls back to a plain slider and then becomes
a waveform, which is what it already did. A track change during the cover
skips the walk entirely, since the request for the new one is already
sitting there.

**What makes a second drawing task allowable is that `gfx.c` now
serialises blits properly** -- a mutex, and a wait on the panel's
completion callback. Before that, "one writer to the framebuffer" was true
only because the tasks that drew happened to take turns by construction,
which is not a property you can add a task to. The two writers still own
disjoint rows: `media_task` paints the artwork area and `ui_task` the bar.

**A repaint is not a track change.** `load_track_visuals()` runs for two
reasons and they do not want the same work: a new track needs everything,
while a repaint -- the chooser closing, having drawn over the artwork --
needs the cover put back and nothing else, because the envelope on screen
is already this track's. Without that distinction, dismissing the chooser
cost a full walk of the playing file. Cancelling out of a folder with
nothing playable in it read 30 MB off the card to produce an envelope
identical to the one already drawn:

```
/usb/FirmamentSoundtrack: 0 tracks
nothing playable in /usb/FirmamentSoundtrack
tags: "Doctor" / ...                  <- repaint of the playing track
walk: 6043 frames, 157s, levels=1     <- and its envelope, again
```

So `media_task` remembers the path it last walked and skips the walk when
it matches.

**A track changes when it is chosen, not when it is understood.** These
were the same moment -- invalidation lived at the top of
`load_track_visuals()` -- and `load_track_visuals()` runs *after*
`decoder_open()`, which on a Xing-less MP3 is a full scan of the file.
Twelve seconds on a USB drive. For all of it the screen kept the outgoing
track's title, artist and cover, and anything the background task had in
flight for the old track still counted as current:

```
playing /usb/aom/hotlantis.mp3          <- new track
cover is 1920x1920                      <- previous track's cover,
cover fitted to 720x720                    decoded and drawn anyway
...
no ID3 text frames; showing the filename   <- 3.3 s later
```

The generation check was not wrong there, it was late: at the moment that
cover was drawn, the new track had not yet reached the line that bumps the
counter, so the cover was current by the only definition available.
`track_change_begin()` now runs before `decoder_open()` -- counter,
scan abort, tags cleared to the filename, artwork blanked -- so the screen
goes honest immediately instead of lying for as long as the open takes.

**A decode cannot be cancelled partway**, the way a frame walk polls a
flag and stops. So track identity is a counter. `media_task` copies
`s_track_gen` before it starts and checks it twice -- after the read, and
after the decode -- and bins what it holds if the number has moved. The
envelope does not need this because it is drawn once from a flag the
decode loop owns; a cover blitted late has no later redraw to correct it.

### The decoder is baseline-only, and should say so

A Frostpunk cover -- a valid 511 KB JPEG -- produced:

```
E jpeg.decoder: SOS encountered before SOF0
E tab5_art: albumart_draw(261): jpeg info
W tab5_mp3: cover art failed to decode (ESP_ERR_NOT_FOUND)
```

That is a progressive JPEG. The P4's decoder handles SOF0 (and SOF1)
only, so handed a progressive file it walks the markers, never finds a SOF
it knows, reaches the scan and complains about the order of the markers.
Which is accurate in the way a stack trace is accurate: it reads as a
corrupt tag or a bug in this file, and it is neither -- the file is a
perfectly good JPEG that this silicon cannot decode.

So `albumart_draw()` walks the markers itself first, in about fifteen
lines, and says which flavour it found:

```
W tab5_art: cover is a progressive JPEG (SOF marker 0xC2); this decoder is
            baseline-only
```

Worth the lines because that answer tells you what to do about it --
re-encode the cover as baseline -- and the driver's version does not. It
also covers the lossless, differential and arithmetic-coded SOFs, which
fail the same way for the same reason and are rarer only by luck.

### One blit at a time

The claim that there is a single writer to the framebuffer was never quite
true. The transport bar is drawn by `ui_task` and the artwork by the
decode loop -- two tasks -- and both end in `esp_lcd_panel_draw_bitmap()`.
The DPI panel takes one transfer at a time and says so:

```
E lcd.dsi: dpi_panel_draw_bitmap(553): previous draw operation is not finished
```

It was rare while the bar repainted at 10 Hz and stopped being rare the
moment a bouncing title raised that to 25.

A mutex in `gfx_blit_err()` is necessary and not sufficient. Drawing from
an external buffer goes out over DMA2D and **returns before the transfer
completes** -- the driver takes its own semaphore with a zero timeout and
returns `ESP_ERR_INVALID_STATE` if the previous one is still in flight --
so a second caller can lose even after the first has returned.

The first attempt at that was a bounded retry, and it worked and was loud:
the driver logs an error from inside on every attempt that loses, so a
contended blit printed three or four lines before succeeding. Retrying an
operation that has a completion callback is guessing at a fact the
hardware will tell you. So `on_color_trans_done` is registered and each
blit waits for it **before releasing the mutex**, which means the next
caller cannot be early. The callback has to live in IRAM; the driver
checks, because it is called from the DMA completion ISR.

The retry stays as a fallback with a 60 ms wait behind it. If the callback
is ever not delivered -- a driver path that skips it, a timeout -- the
behaviour degrades to what it was rather than to a stall.

The two writers own disjoint bands of the shadow, rows above the bar and
rows below it, so the transfer is the only thing they contend for.

### The scan is cancelled before the cover is decoded, not after

`load_track_visuals()` cancelled the outgoing track's frame walk at the
bottom of the function, after the cover had been decoded. Decoding a cover
can take seconds, and during those seconds the previous track's walk ran
to completion against a card the audio decoder was also reading:

```
playing 04 - The Factory.mp3
walk: 5957 frames, 142s          <- 06 - Processing's walk
envelope ready: 720 columns
```

The result was discarded correctly a few seconds later, so nothing wrong
appeared on screen. What it cost was the seconds of contention that
produced it. The abort is the cheapest statement in the function and there
was no reason for it to be last.

### Primitives moved to gfx.c

The chooser needs rectangles, circles, seven-segment digits and clipped
`font8x8` text, which is exactly the set `ui.c` had as statics. They moved
to `gfx.c` unchanged apart from the name, for the reason the README
already gives for `id3_read_tags()` living in `albumart.c`: a second copy
is the thing that drifts.

`gfx.c` also owns the framebuffer lookup and the blit, so `ui.c` and
`browser.c` both stop caring which panel handle is which.

### Written against the documented API again

The USB half of this has the same caveat the decoder stack carries: it is
written against the shape of `usb_host` and `espressif/usb_host_msc`
rather than against a board.

- **Port selection is not configured here.** The Tab5 wires USB-A to the
  P4's `USB2_OTG` D+/D- -- the high-speed controller, not the
  USB-Serial/JTAG the USB-C port uses for flashing -- and the default is
  expected to land there. If nothing enumerates with VBUS confirmed high,
  this is the first thing to doubt.
- **Full-speed devices are a known IDF bug**, fixed in 5.4.2. Below that,
  a full-speed drive fails with `Root port reset failed` every ~2.3 s
  while high-speed devices work. `idf_component.yml` already floors at
  5.4.2.
- **Bus power is 2.0 only.** A drive that wants more than the port will
  give brown-outs rather than failing to enumerate. Spinning rust needs
  its own supply.

### What the controls do not do yet

- ~~**Seek on non-MP3 works only where the byte rate is provably
  constant**, per above: PCM WAV, CBR ADTS, fixed-mode AMR.~~ **Every
  format this player decodes is seekable as of 0808**, by five
  mechanisms: a proven-constant byte rate (WAV, CBR ADTS, AMR), a
  bisection over frame headers, page granules or PES timestamps (FLAC,
  Ogg, TS), a sample table with the audio remuxed to ADTS (AAC in MP4),
  the same table feeding frames straight to the decoder (ALAC in MP4),
  and a table walked out of the frame headers for a stream that has
  none of the above (VBR ADTS, 0805). There is no longer an exception.
- **The marquee is pixel-stepped, not eased.** It starts and stops at full
  speed. Easing needs a curve and a frame counter for a 3 px/frame slide,
  which is more state than the effect is worth.
- ~~**Non-Latin still shows as boxes**, marquee or not. ark10 stops at
  Latin Extended-A.~~ Fixed by the move to ark12; see "The font is 12px
  now" below. Cyrillic, kana and CJK all render. A few individual CJK
  codepoints are still boxes because Ark has no glyph for them at any
  size, which is a gap in the font rather than in the table.

### Licensing, since you already care about this for exFAT

- minimp3 is CC0/public domain. No attribution obligation, vendored
  anyway so the source is auditable in-tree.
- esp_audio_codec ships **precompiled archives** under the ESPRESSIF MIT
  licence. Free, but the grant is limited to Espressif silicon. Fine
  here; worth knowing before this code gets copied somewhere it is not.
- pngle and miniz are MIT.
- **Ark Pixel Font is SIL OFL-1.1, and `components/ark12` is therefore
  OFL-1.1 too, not MIT.** Converting the glyph PNGs into C arrays makes
  those files a Modified Version under OFL section 5, and section 5
  requires Modified Versions to stay under the OFL. This is not a problem
  -- the OFL explicitly allows bundling with software under any licence,
  and only the font files are bound -- but `components/ark12/LICENSE-OFL`
  has to ship with any redistribution, including a firmware image, and
  the tables must not be sold on their own. Ark declares no Reserved Font
  Name, so the derivative did not have to be renamed; it is called
  `ark12` anyway, because it is not the Original Version.

  This is the one obligation the project did not previously have. font8x8
  was public domain and nothing had to travel with it.

### Open, and things that only look open

The README no longer carries the TODO list this section used to mirror,
so this is the list. Entries are struck through rather than deleted:
which of these turned out to be finished, and which turned out never to
have been a task, is the useful part of a list like this one.

- ~~**Cover art is ID3v2-only.**~~ Closed. `covertag.c` dispatches on
  magic bytes and reads FLAC `METADATA_BLOCK_PICTURE` and M4A `covr` as
  well. This entry outlived the work by several patches, which is the
  ordinary failure mode of a list like this one.
- ~~**Screen sleep** is still just the backlight.~~ Not open, and not
  closable either. **The backlight is the ceiling, because touch is the
  only way back.** `TP_RST` is expander 1 P5 -- the same pin, driven by
  the same `PI4IOE1_OUT_SET` write, that releases `LCD_RST`. Touch and
  panel come up together and there is no way to hold one down and keep
  the other alive. Take the panel down and the wake tap has nothing left
  to read it, so the device is off rather than asleep. Taking the decoder
  down is a separate and available thing, but it is called stopping the
  music, and a listener who wanted that pressed pause. Listing this as
  pending implied a design nobody had found yet; the wiring is the
  answer.
- ~~**exFAT is still a script, not a default.**~~ Closed, and closed some
  time ago. `cmake/exfat.cmake` runs `tools/enable_exfat.sh --no-clean`
  at configure time, included from the top-level `CMakeLists.txt` before
  `project()` so the component exists by the time IDF scans for Kconfig.
  A fresh clone gets exFAT without knowing the script is there;
  `TAB5_NO_EXFAT` is the opt-out and `--revert` is the undo. The file
  that documents that behaviour is the header comment of
  `cmake/exfat.cmake`, which this list contradicted for several patches.
- **`.m4a` is a container.** AAC and ALAC inside it work; encrypted
  audio does not and cannot. As of 0814 it is refused at open with a
  line saying so, rather than opening and failing on the first frame:
  `mp4_probe()` recognises `drms`, `enca` and `aavd` in the sample
  entry and sets `mp4_t.encrypted`, which is the one field there that
  survives `mp4_free()`. This is a better error message and nothing
  more -- there is no key here and there is not going to be one.
- ~~**Nothing is peak-limited.**~~ Closed by 1006, in two halves and
  neither of them a limiter. ~~FLAC and WAV arrive at full scale where
  MP3 rarely did~~ was measured in 1004 and is false. **Real music does
  reach the rail** -- a 320 kbps MP3 reports `output peak 32768/32768
  (0.00 dBFS)` -- and it still does not clip, because that only happens
  on the unity pass while measuring, and every play after it applies
  -11.96 dB. `clamp_hits` has never fired. See "Nothing digital can clip
  here".

  **This is not the same as FLAC and WAV missing ReplayGain.** They do
  not miss it. `loudness.c` measures the decoded int16 block on its way
  to the ring, which is downstream of every backend, so the measurement
  never sees a container or an extension and cannot have a per-format
  gap. `measuring` in `player.c` is `!known` -- whether a sidecar already
  holds an answer -- and nothing else. A lossless track that plays at a
  level a lossy one did not is the format being louder, correctly
  measured, and arriving at an output stage with no limiter after it.
  Those are two facts about the same track and only the second one is a
  missing feature.

### The ffmpeg vs python-script item

With this patch the transcode advice narrows a lot. `.mp1`/`.mp2`
mislabelled `.mp3` decode natively, and Opus arrives through the Ogg
parser, so the cases that genuinely need re-encoding are ALAC, Vorbis and
anything DRM'd. The first two are only a framing layer away rather than a
codec away. Everything else is a decoder problem, and decoder problems
get fixed in `decoder.c`.

## Licensing: the font is not MIT

**`components/ark12/` is SIL OFL-1.1. Everything else in this repository
is MIT.**

`components/ark12/ark12.c` and `ark12.h` are generated from
[Ark Pixel Font](https://github.com/TakWolf/ark-pixel-font) — copyright
(c) 2021, TakWolf, licensed under the SIL Open Font License, Version 1.1.
Converting glyph PNGs into C arrays is a format change, not a rewrite, so
those files are a *Modified Version* of the Font Software under the OFL,
and **OFL section 5 requires Modified Versions to remain under the OFL.**
They cannot be relicensed MIT. Do not add an MIT SPDX line to them, and
do not "clean up" the OFL-1.1 line that is there.

What this actually obliges:

- **`components/ark12/LICENSE-OFL` ships with any redistribution** of
  those files or of a binary built from them. A flashed firmware image
  counts as distribution. If this project ever grows a release artifact,
  the licence text goes in it.
- **The tables must not be sold on their own** — only bundled. The OFL
  explicitly permits bundling with software under any licence, including
  commercial and including MIT, so the rest of the project is unaffected.
- **No renaming required.** Ark Pixel declares no Reserved Font Name.
  The component is called `ark12` rather than `ark-pixel-font` anyway,
  because it is a subset in a different format and should not be mistaken
  for the Original Version.
- **Attribution stays in the generated headers.** `tools/gen_ark12.py`
  writes the copyright and licence notice into every file it emits. If
  you change the templates in that script, keep those blocks.

This obligation is new. The previous font, `font8x8`, was public domain
and nothing had to travel with it — which is why the README's licensing
section did not previously mention fonts at all.

Other third-party terms, unchanged: minimp3 is CC0; pngle and miniz are
MIT; esp_audio_codec is precompiled under the ESPRESSIF MIT licence,
whose grant is limited to Espressif silicon.

## The font is generated, not vendored

`cmake/vendored.cmake` fetches minimp3 and pngle at configure time and
checks a SHA256 per file. The font does not go through that path and
cannot: Ark ships one PNG per glyph and builds its font files at release
time, so there is no single file to pin.

So `components/ark12/ark12.{c,h}` are **committed**, and regenerated by
hand with:

    ./tools/gen_ark12.py

Standard library only, no Pillow. `ARK_COMMIT` at the top of that script
is the pin, and it is a real commit sha — the committed table reproduces
byte for byte from it, which is the only thing that makes committing
generated output honest. Never hand-edit the generated files; the next
regeneration silently discards the edit.

To add coverage, add the block to `RANGES` and rerun — but **read the
per-range counts the script prints**. A range the font does not draw at
this pixel size contributes zero glyphs and still produces a clean build,
which is this script's real failure mode and how the 10px attempt at CJK
would have gone. The table is currently ~545 KB, nearly all of it CJK
Unified; dropping `(0x4E00, 0x9FFF)` takes it to about 62 KB.

## Text is UTF-8 end to end

Three things have to agree, and did not before the font swap:

1. **`sdkconfig.defaults` sets `CONFIG_FATFS_API_ENCODING_UTF_8`.**
   Without it `readdir()` returns codepage 437 bytes and every accented
   filename is mojibake — the tags would be right and the browser wrong,
   which is worse than both being wrong.
2. **`albumart.c:id3_text_to_utf8()`** converts all four ID3 text
   encodings. Encoding 0 is **Latin-1, not ASCII**: byte 0xE9 must be
   widened to a two-byte `é`, not copied. Copying it is the classic
   silent ID3 bug.
3. **`gfx.c` decodes UTF-8 to codepoints** and looks them up in ark12.
   Invalid bytes cost one notdef box each and do not desync the decoder.

If you add a new source of display strings, it has to produce UTF-8.

## Glyph metrics changed with the font

`font8x8` was 8×8 with a 9 px advance. `ark10` was 5×10 with a 6 px
advance. **`ark12` has two cells**: 6×12 halfwidth with a 7 px advance,
and 12×12 fullwidth with a 13 px advance.

- Use `GFX_GLYPH_W(scale)` and `GFX_GLYPH_H(scale)`. A literal `8 *
  scale` for vertical centring was correct for `font8x8` and is wrong for
  everything since.
- **`GFX_GLYPH_W(scale)` is the *halfwidth* advance, not "the width of a
  glyph".** It is still correct for tab labels, digits and static UI
  strings, which are Latin by construction. It is *not* correct for
  anything that can hold a tag or a filename, because those can contain
  fullwidth glyphs that advance by `GFX_GLYPH_W_FULL(scale)` instead.
- `gfx_text_w()` sums **per-glyph advances**, not bytes and not a cell
  count. Do not substitute `strlen()` for it — `"Rós"` is four bytes and
  three cells — and do not substitute `count * GFX_GLYPH_W(scale)` for it
  either, which is the newer version of the same mistake.
- `gfx_draw_char()` takes a `uint32_t` codepoint, not a `char`.
- `ui.c`'s marquee compares `strlen()` as a change-detection token only.
  That is still fine: it is a token, not a width.

## A request needs a reader

`s_seek_pct` is a request to the decode loop. At the end of a playlist
there is no decode loop -- so a seek sat in the variable until an
unrelated track started and was then applied to it, fifteen seconds
after the button. From the outside: four presses that did nothing,
followed by a song that mysteriously started from zero.

`s_decoding` says whether a reader exists. Seeks are **refused** when it
is false, not queued -- a request with no reader is not pending, it is
lost, and a lost request that fires later against a different song is
worse than one that never fires. Pending seeks are also dropped on any
track change and when a track ends.

Set it after `decoder_open()` succeeds, clear it on **every** exit from
`play_file()` including the out-of-memory path. Leaving it true on an
error path reintroduces the bug in the place it is hardest to see.

## Heap corruption: what actually happened

Symptom: `tlsf_free ... block already marked as free`, backtrace always
pointing at the PCM ring's delete. Two theories built on that backtrace
were wrong. What resolved it was patch 11 reverting the ring from 256 KB
of PSRAM via `xStreamBufferCreateWithCaps()` back to 64 KB via plain
`xStreamBufferCreate()` -- after which the same rapid-skip sequence that
crashed twice ran for three and a half minutes clean.

**So: do not put the PCM ring in PSRAM with the WithCaps API.** The
mechanism is not established -- absence of a crash in one long run is
strong evidence, not proof.

**This is still the rule and the ring is nevertheless 3520 KB in PSRAM,
which is not a contradiction.** What was banned was the *allocator*, not
the memory. The storage is allocated once in `app_main()` and never
freed, and `xStreamBufferCreateStatic()` sits on top of it, so there is
no `WithCaps` create to mispair with a plain delete and nothing on the
per-track path allocates at all. The way to get the size back was to
remove the allocation from the hot path rather than to argue that the
allocator was innocent.

The ring is created and freed once per track, which makes it the
most-churned allocation and therefore the block that discovers damage
first. It will appear in the backtrace of almost any heap corruption
here and says nothing about the cause. Bisect with `-DHEAPCHECK=1` and
`CONFIG_HEAP_POISONING_COMPREHENSIVE` instead of reading the stack.

## Debugging heap corruption here: read this first

The PCM ring is created and freed once per track, which makes it the
most-churned allocation in the program and therefore **the block that
keeps discovering damage somebody else did.** It will appear in the
backtrace of almost any heap corruption, and it says nothing about the
cause.

Two theories were built on that backtrace and both were wrong: a
use-after-free on the ring handle (real, fixed, not the cause) and the
larger PSRAM ring (not a bug at all). Do not add a third by reading the
stack trace.

Instead:

1. `idf.py -DHEAPCHECK=1 build`. `main/heapcheck.h` checks every heap at
   named points -- around `do_art`, `do_walk`, prefetch, and ring
   create/delete -- and logs the first one that fails. The last good
   checkpoint and the first bad one name the subsystem.
2. Turn on `CONFIG_HEAP_POISONING_COMPREHENSIVE`
   (Component config -> Heap memory debugging -> Comprehensive). This
   catches the offending WRITE rather than the next free, which is the
   difference between finding it and guessing.
3. Ruled out so far by host testing under ASan: `mediacache.c` (2000
   rapid track changes with the full ownership dance, clean),
   `covertag.c` (1200-case fuzz corpus, clean).

The checkpoints do not abort on failure, deliberately -- the sequence is
the evidence, and aborting on the first bad one discards what came
before it.

## Never share a handle across tasks; publish a value

`s_pcm` belongs to `play_file()` and the I2S writer, and to nobody else.
The decode loop deletes it at the end of every track, so any other task
holding it is one context switch away from reading freed PSRAM.

The first version of the prefetch gate let `media_task` call
`xStreamBufferBytesAvailable(s_pcm)` behind an `if (!s_pcm)` guard. That
guard does nothing: the pointer can be loaded before the check and used
after the free. It crashed on hardware exactly where that predicts --
`next` pressed while the frame walk was running, so `media_task` was
polling occupancy every 100 ms -- and the symptom was a TLSF assert on a
later, unrelated `free()`, because what a stray read corrupts is heap
metadata rather than anything of ours. Reproduced under ASan as a
heap-use-after-free on the first run.

**The rule: cross-task, publish a number, not a pointer.** `s_ring_pct`
is written by the owner and read by anyone; an int cannot dangle. And
teardown order is publish-invalid, clear-handle, then free, so there is
never a reachable handle to a freed buffer.

If anything else ever needs to know about the ring, it gets another
published value. It does not get `s_pcm`.

## Control latency lives in the decode loop

Seeks and track changes are requested on the UI task and serviced at the
top of the decode loop, so **the decode loop's worst-case iteration time
is the control latency.** Anything that stalls that loop presents as a
button that did nothing, and the user presses it again, which makes it
worse.

Three things guard it now, and all three log:

- `request_seek()` warns when a request overwrites one that was never
  serviced. `s_seek_pct` is a single slot, so rapid presses collapse --
  and without the warning that is indistinguishable from a dead button.
- The ring send is sliced at `SEND_SLICE_MS` rather than blocking on the
  whole block, so ring size cannot become control latency.
- `LOOP_STALL_MS` timing around `decoder_read()` and the ring send says
  which of the two stalled. From outside they are the same symptom.

A note on a wrong theory, so it does not get re-derived: enlarging the
ring does **not** increase this latency. The writer drains continuously,
so a send waits only for room for one block -- about 27 ms at 64 KB and
about 27 ms at 512 KB. This was measured, after being asserted
incorrectly.

## Priority is not a device throttle

`media_task` runs at priority 1 and that is not what keeps it out of the
decoder's way. Priority arbitrates the CPU; a 512 KB read issued at
priority 1 sits in the same I/O queue as the decoder's next refill.
Background work is throttled by `media_settle()` -- a delay, then a
ring-occupancy floor, then a bounded timeout -- not by being scheduled
politely.

If background reads ever need to be added, they go through
`media_settle()` too. Lowering a priority instead will look like it
worked on SD and fail on USB.

**As of the arbiter this is half the answer.** `media_settle()` decides
*when to start*; it has never had anything to say about a read already in
flight. See below.

## The card is arbitrated, not throttled

`storage_io.c` is a lease on the device, taken per read, granted by class
rather than by task priority:

| Class | Who |
| --- | --- |
| `STORAGE_IO_PLAYBACK` | the decode loop, and nothing else |
| `STORAGE_IO_PREFETCH` | the next track's tags, cover, envelope |
| `STORAGE_IO_BACKGROUND` | the playing track's envelope, listings |

Lower wins. The failure it removes is the one `ring_publish()` already
describes and could only report: `media_task` enters a 32 KB `fread`,
FatFs takes the volume lock, the decoder's next refill blocks behind it,
the decode loop stops, and because the decode loop is what publishes
`s_ring_pct` the gauge freezes above the abort threshold -- so the abort
never fires. Adding the writer task as a second publisher made that gauge
honest. It did not stop the read.

**The lease does not make background reads shorter. It makes them
interruptible.** `storage_io_fread()` reads exactly what it was asked for,
in `STORAGE_IO_CHUNK` pieces, dropping the lease between them. The
decoder's worst case becomes one chunk of the current device rather than
the rest of somebody else's file.

Three things worth not undoing:

- **Wrap the read, never the parse.** `covertag.c` and `duration.c` both
  funnel every read through their own `read_at()`, which is why each is a
  one-function change. Wrapping `covertag_extract_art()` instead would
  have been one line in `player.c`, would have compiled, and would have
  put the starvation straight back -- one lease across a 512 KB cover is
  the uninterruptible read this exists to break up. `storage_io_acquire()`
  detects that nesting and logs it, once, rather than deadlocking on it:
  a hang on the decode loop is worse than a wrong-but-working read.
- **The wake is not the grant.** Releasing gives a binary semaphore, which
  wakes one waiter, and FreeRTOS picks that one by task priority -- the
  thing this file exists to not decide by. So every waiter also re-tests
  on a 4 ms timer and correctness comes from the re-test. The cost is 4 ms
  of grant latency when the wrong task is woken, against a ring holding
  tens of seconds.
- **`storage_io_stats()` is the thing to read when this looks wrong.**
  PLAYBACK's worst wait should be roughly one chunk of the current device.
  If it is not, somebody is holding a lease across a parse.

Small reads are deliberately left unarbitrated -- the ten-byte ID3 frame
headers in `albumart.c`, and the sidecar's own kilobyte. A lease
costs two semaphore operations and those reads are shorter than that. Only
`albumart.c`'s APIC frame, which is the cover itself, takes one.

## The envelope scan is off, and what the first flash showed (historical)

`WAVEFORM_SCAN` no longer exists; the feature it gated is unconditional
because it costs nothing beyond a play you were having anyway. The
measurement below is kept because it is the number that eventually killed
the walk.

The first flash of the arbiter measured this, on an 8.7 MB Xing-less MP3:

    press -> sound          15.4 s on the first track, 18.9 s on the second
    decoder_open()          15.4 s of it
    framewalk_scan()        a second whole-file pass, reporting 273 s

Both passes read the same file. `MP3D_SEEK_TO_SAMPLE` reads it to build
minimp3's sample index, which yields the frame count and the duration; the
walk then read it again for the frame count, the duration and the
envelope. Two of the three answers were already known.

The conclusion drawn at the time was to switch the walk off and bring it
back behind a sidecar, so the second read became once-per-file instead of
once-per-play. That was the wrong shape and it took until 0206 to see it:
the second read did not need to be cached, it needed to not exist. The
decode loop already has the PCM, so the envelope is free there and the
walk had nothing left to do.

Worth stating because the intermediate answer was reasonable and shipped:
0200 built the sidecar, keyed it on size and mtime, wired it to the walk,
and worked. It just cached the result of a pass that should not have been
happening. "Make the expensive thing cheaper" and "notice the expensive
thing is redundant" look identical until someone asks what else already
has the data.

## What the log now says

Three additions, all aimed at the gap between a press and a sound:

- **`open took N ms`**, timed around `decoder_open()` alone.
- **`first sound N ms to sound`**, spanning `track_change_begin()`, the
  open and the first decoded block. Logged on the first block only.
  Reworded in 0908: it used to say "after the press", which is a small
  lie on an automatic boundary where nobody pressed anything, and it
  used to be described here as "the interval during which the screen is
  blank and nothing plays", which is false at a rate change -- the drain
  is the previous track being heard. When a drain happened the line now
  states it separately, so the headline figure keeps meaning what it
  says.
- **`storage_io_report(phase)`** at three points: after the open, after a
  failed open, and at the end of the track. The phase label is what makes
  the numbers mean anything, because the counters reset on read: the
  `open` window is measured against an otherwise idle card, and the
  `track` window is steady-state playback with whatever background work
  got in alongside it. Playback's worst wait in the second window is the
  arbiter's actual result.

`bytes` was added to the stats for this. A read count says how often the
lease changed hands; the byte total says what came off the card, and it is
the figure that identifies a whole-file pass nobody asked for. Expect the
open on a Xing-less MP3 to report roughly the size of the file.

## Bytes over wall-clock is not a throughput, and 0101 said it was

0101's report gave 8551 KB read during a 15417 ms open, and that got read
as 555 KB/s. It is not the card's rate. The open is a read and a parse
taking turns -- minimp3 walks every frame header between buffers -- so
bytes over wall-clock is the average of the two and attributes all of it
to the card.

0102 measures the lease instead. `held_ms` is time actually inside
`fread()`, stamped at the outermost acquire and closed at the outermost
release, and `bytes / held_ms` is the card with the parsing taken out.
The line now reads:

    open playback: 77 reads, 8551 KB in N ms held of 15417 ms (P%),
                   K KB/s, worst hold H ms, worst wait 0 ms

and the three figures answer three different questions:

| Figure | Says |
| --- | --- |
| `K KB/s` | how fast the card actually is |
| `P%` | how much of the window was I/O at all -- the rest is minimp3 |
| `H ms` | the floor on control latency |

**`H` is the one with a consequence attached.** The decode loop cannot
look at a button while it is inside a read, so the longest single
uninterruptible read is the shortest a button press can possibly take.
0101 logged `seek (slider) waited 229 ms for the decode loop` and that is
what it was. `SEND_SLICE_MS` slices the ring send at 20 ms against a ring
that is not the bottleneck.

The prediction worth writing down before the flash: if `P` is high, the
card is slow and `MP3D_DO_NOT_SCAN` buys back only what a later walk would
spend anyway. If `P` is low, the open is CPU-bound in minimp3's index
build and `DO_NOT_SCAN` buys the whole fifteen seconds. Those want
different patches, which is why 0102 is measurement and 0103 is not.

## The card is mounted at High Speed, and falls back

`SDMMC_FREQ_HIGHSPEED` (40 MHz) rather than `SDMMC_FREQ_DEFAULT` (20),
which at 4-bit moves the bus ceiling from 10 MB/s to 20. The card in the
test rig is SDHC, a class that supports High Speed; the previous value was
half the bus for no stated reason.

The frequency is **latched, not re-probed**. `sd_mount()` is the 1 Hz
removal poll, and an empty slot fails it by timing out -- so a blind "try
fast, then try slow" would double the cost of the commonest state in the
program, which is nobody having put a card in, and would re-probe 40 MHz
once a second forever on a board that cannot do it.

Only a card that answered and then failed is evidence about the clock.
`ESP_ERR_TIMEOUT` and `ESP_ERR_NOT_FOUND` are an empty slot and say
nothing; `ESP_FAIL` is "no mountable filesystem", which is a formatting
problem that would produce the same complaint at half the speed. Anything
else drops to 20 MHz for good and says so.

The `speed` line in the mount banner is `s_card->max_freq_khz`, which is
what was negotiated rather than what was asked for, so a card that
declines High Speed reports what it settled on.

## Two more numbers, and why each exists

- **`mp3: index built, N samples, N s, seekable`** in `decoder.c`, so
  what the open's fifteen seconds bought is on one line next to what it
  cost. `ex.samples` counts int16 values across all channels, the same
  units `mp3dec_ex_seek()` takes, so the frame count is samples over
  channels -- dividing by the rate without dividing by channels first
  reports half the duration of a stereo file.
- **`ring N%` on the first-sound line.** The decode loop has had the
  entire open to fill the ring and the writer has not started draining
  it, so a low number is the decoder losing a race it began with a head
  start. That is the shape any gapless prefetch has to fit into.

Deliberately **not** in 0102: the prefetch gate regression 0101 caused
(`prefetch held off: ring at 61%, need 75%`, every track, because
removing the walk let `media_task` reach the gate while the ring was
still cold). Fixing it would put a second reader back on the card during
playback and change the numbers this patch exists to collect. It is 0103,
and it is the thing that will finally give the arbiter a contended window
to be judged on -- every `worst wait` in the 0101 log is 0 ms, because
the only concurrent reader was the walk and 0101 switched it off.

## Where v0.2.0 got to, and what is next

**Read this first if you are picking this up cold.** Patches 0100-0105
were one session. The short version: the player was slow because
`fread()` was reaching the filesystem in `BUFSIZ` pieces, and everything
before 0104 was looking in the wrong place for it.

| | before | after 0105 |
| --- | --- | --- |
| Card throughput | 576 KB/s | 7823 KB/s |
| Open, 8.5 MB MP3 | 14938 ms | 1218 ms |
| Press to sound | 15336 ms | 1572 ms |
| Longest single read | 219 ms | 21 ms |
| Card busy during playback | 10% | 0.85% |

`st_blksize` came back **0**, not 512 as predicted, so newlib fell
through to `BUFSIZ`. 7823/576 = 13.58, and 16384/13.58 is about 1206
bytes -- `BUFSIZ` plus overhead. The mechanism was right and the constant
was wrong.

### The four things v0.2.0 set out to fix

1. **Display flashing cyan.** Fixed, 0500-0509, and it was never a
   bandwidth problem. Every assumption in this line was wrong: see
   "The cyan flash was xStreamBufferReset()".
2. **The card starving other tasks.** Arbitrated in 0100. Briefly
   validated and then made moot. With the walk running in the background
   against a playing track, logs showed `worst wait` reaching 39 ms and a
   background class doing 19 MB of reads across one track -- the first
   real contention the arbiter ever saw, and it held.

   The walk is gone as of 0206, and with it the only long read that ran
   against playback. `worst wait` went back to 0-1 ms because nothing
   contended any more: what was left was the open, the ring's own
   refills, and a kilobyte of sidecar.

   **It is a working part again as of 1006.** A 3.7 MB PNG cover being
   prefetched against playback is a long read on the device the decoder
   is using, which is exactly what the arbiter was built for, and it held
   -- 234 ms of worst wait rather than the whole cover.
3. **Gapless needs RAM caching.** Not started, but no longer blocked: it
   was impossible against a 19 s open and is merely unwritten against a
   1.5 s one.
4. **ReplayGain envelope in a sidecar.** Done, 0200-0212, and it went
   further than this entry imagined. The sidecar exists (`.<name>.rgcache`,
   JSON Lines, one line, keyed on size and mtime), but the whole-file
   walk it was meant to amortise is gone rather than cached: real
   BS.1770 loudness and a real amplitude envelope are both measured off
   the PCM that playback already decodes, so the first uninterrupted
   play produces them and no pass over the file is ever scheduled.
   `WAVEFORM_SCAN` is gone with the walk -- the feature is unconditional
   because it costs nothing beyond a play you were having anyway.

   The record also now carries tags, the format the decoder reports, and
   whether the file has cover art, so a second play answers all of them
   from one read instead of an ID3 parse and an eight-to-thirteen second
   tag scan.

### What to do next, roughly in order

- ~~**Check `worst wait` now that prefetch runs.**~~ Answered in 1006,
  and the arbiter was worth it. Prefetching a 3.7 MB PNG cover against
  playback produced `worst hold 387 ms, worst wait 234 ms` on PLAYBACK,
  against 0-1 ms in every log since the walk was deleted. The lease held
  and broke the read into chunks. What it also showed is that 234 ms is
  not "roughly one chunk of the current device", which is what "The card
  is arbitrated, not throttled" predicts -- see the cover cache note.
- ~~The seek index.~~ Wired in 0703. What is left to check is the
  number it trades for: the seek timing `decoder_seek_sec()` now logs.
  A coarse table moves the cost from every play to every seek, and
  whether that is the right spacing is a measurement nobody has taken
  yet.
- **Gapless**, which wants the next track decoding before the current one
  ends. At 0.85% card duty there is room; the ring is 3520 KB -- about
  20 s -- and reaches 0% at first sound, so the head start has to be
  built rather than assumed.
- ~~`MP3D_DO_NOT_SCAN` is now the only thing between a press and
  sound.~~ Taken in 0703, on the second and later plays of a file. The
  first play of a Xing-less MP3 still scans, because the table has to
  come from somewhere and a walk the player is already doing is the
  cheapest place for it to come from.

### Things left deliberately broken or unfinished

- ~~`covertag.c` is `PREFETCH` class for every caller, including the
  decode loop's own `load_tags()`, which should be `PLAYBACK`.~~ Closed
  by 0906, which threaded the class through as a parameter. This entry
  outlived the work by the whole 0900 series and was still being read as
  open at the v0.3.0 review -- the same failure 0811 and 0812 both
  record, in the list that documents it.
- ~~`duration.c` is `PREFETCH` for every read.~~ Closed by 1000, which
  is the same fix from the other side, and confirmed on the board by
  1001 -- the Ogg probe's 64 KB window is absent from PREFETCH's
  per-track total. See below.
- One lease covers the SD card and the USB port together.
- ~~Raw ADTS and AMR have no duration on a track that has never been
  played through.~~ Fixed: `cbrseek.c` derives it from a proven-constant
  byte rate at open, for the CBR case, which is what those two formats
  are in practice. A genuinely VBR ADTS file still reads `--:--` until a
  full play records a duration in the sidecar.
- The size-only handle in `do_art()` is deliberately left on plain
  `fopen()`: it opens, seeks, tells and closes without reading a byte, so
  a pool slot spent on it is a slot the decoder cannot have.
- Nothing in 0100-0105 was compile-tested against ESP-IDF. `storage_io.c`
  is clean under `gcc -Wall -Wextra -Wformat=2`.
- The 0200 series is host-tested, not IDF-built, and shipped two build
  failures because of it: a struct field removed in one patch and still
  referenced in another, and four functions called above their
  definitions. Both are classes an isolated harness structurally cannot
  see -- it compiles function bodies, not the file they sit in. Type
  checks against the real headers catch the first; a use-before-definition
  scan catches the second; neither substitutes for a build.
- ~~`waveform_draw_flat()` and `draw_slider()` were removed as dead in
  0206; `framewalk_t` still carries `frames` and `has_levels`, which
  nothing fills any more.~~ Closed by 1001, which found the entry wrong
  in both directions: `has_levels` is live, `rate` was dead and unlisted.
  `frames` and `rate` are gone; `has_levels` stays and says why in
  `framewalk.h`.

### The method that actually worked

Three hypotheses were tested and two were wrong: the bus clock (0102) and
DMA capability of the destination (0103). Both were plausible, both were
measured, both moved throughput by about 1%. What made the third one
findable was that each failed patch **narrowed** where the problem could
be -- invariance to the clock and to the destination put it above the
driver, which is the only reason `fread()` was worth looking at.

Write the falsification condition into the patch before flashing it. 0103
said "if throughput does not move and `bounced` is high, this is wrong",
and that is exactly what happened, which turned a wasted patch into a
result.

## The console drops lines, and a log is not a trace (0323-0325)

Two patches went in chasing a stall that did not exist. The evidence was
a missing log line, and the missing log line was the console.

`first sound` stopped appearing on tracks started by a press. It is
logged unconditionally on the first decoded block, there is no `continue`
between the send and it, and the tracks in question decoded eighteen
thousand blocks. Every path that could be read said it must print.

0323 bracketed it: probes before the send and after it. On the failing
runs *neither* fired, which appeared to prove the first block never
reached the send. 0324 narrowed further, into the `cur_rate == 0` branch
-- the only code between the format publish (whose `ogg: ...` line did
appear) and the pre-send probe (which did not). That reading was airtight
and wrong.

The third run of 0324 printed every line and behaved identically to the
two that had not. The lines were never not executed; they were executed
and discarded. `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` drops output when its
TX buffer is full rather than blocking the task that logged it, and a
track start emits six lines inside about 50 ms against a 256-byte
buffer. 0325 raised it to 4 KB.

Three things worth keeping from that:

  - **A missing line is not an unexecuted line.** Absence of output is
    evidence about the console as much as about the code. The tell is
    shape: dropped output goes in contiguous runs with intact lines
    either side, while a stall truncates and stays truncated.
  - **Reproduce before narrowing.** Both probe patches were built on two
    runs that agreed. A third run disagreed and was worth more than
    either.
  - **The buffer defers the loss, it does not remove it.** A host that
    stops draining fills any buffer. If lines vanish again, look for a
    burst first.

Also worth stating plainly because it was asserted twice and was wrong
twice: this was called cosmetic, then called a probable hang on the
strength of a `HP_SYS_HP_WDT_RESET` that followed a monitor disconnect
and had nothing to do with it. Neither claim was measured when it was
made.

## 0103 was wrong, and what the negative result bought

The bounce buffer did nothing. `bounced` came back 77 of 77 and 94 of 94,
so the staging happened exactly as designed, and throughput went from
577 to 576 KB/s. The sector-at-a-time fallback in `sdmmc_read_sectors()`
was never the constraint.

That is worth keeping because of what it rules out. Throughput is now
known to be invariant to **two independent things**: the bus clock
(20 -> 40 MHz, 0102) and the DMA-capability of the destination (0103).
Both live at or below the driver, so the constraint is above it -- not
how fast a request moves bytes but how many requests there are.

576 KB/s is 1127 requests per second at 512 bytes: 0.89 ms per
single-sector round trip through the VFS lock, FatFs and the driver. One
sector per request looks like this at any clock and with any destination,
which is precisely the invariance the two dead patches measured.

**newlib's `fread()` does not pass a large request down.** glibc bypasses
its buffer for reads larger than it; newlib loops on `__srefill_r` and
refills `fp->_bf._size` at a time, so everything reaches `f_read()` one
stdio buffer at a time regardless of what was asked for. minimp3 asks for
128 KB, the arbiter measured 111 KB per lease, and FatFs saw neither.

newlib sizes `_bf` from `st_blksize`, which for this VFS comes from
`CONFIG_FATFS_VFS_FSTAT_BLKSIZE` -- which `sdkconfig.defaults` does not
set. `storage_io_open()` logs the `st_blksize` it finds on the first open,
so this stops being an inference.

`storage_io_open()` is `fopen()` plus `setvbuf()` with a block from a
small preallocated pool of internal, cache-aligned, DMA-capable buffers.
That sets the request size and makes the destination the driver actually
sees DMA-capable -- which is what 0103 was reaching for from the wrong
end. With stdio buffering the bytes land in the stdio buffer first, so
the caller's PSRAM pointer was never what the driver was handed.

0103's staging is removed rather than left in place: on a buffered stream
it is a third copy of every byte, to solve a problem it has been measured
not to solve.

Buffers are preallocated because they are 16 KB internal DMA-capable
blocks and the internal heap is 256 KB with a USB host stack in it.
Taking and returning them at every track change is how that heap ends up
fragmented into a state where the next one fails. Two slots covers
`decoder.c`; a third concurrent open falls back to default buffering,
which is correct and slow rather than broken. A plain `fclose()` on a
`storage_io_open()` handle still closes the file -- it strands a slot
until reboot, and that is the failure worth knowing about if the pool
ever reports empty.

**Only `decoder.c` is converted.** It is where 99% of the bytes are, and
the lesson of 0103 is that the cheap test comes before the rollout.

### The test this has to pass

"A layer below is chunking smaller than we think" is the same class of
hypothesis 0103 was, so it gets the same explicit condition: **if the
stdio buffer is 16 KB and throughput does not move, the constraint is the
card's own per-request latency** and nothing above it will help. At that
point the answer is not to read faster but to read less -- which is
`MP3D_DO_NOT_SCAN`, and it stops being an optimisation and becomes the
only remaining move.

The `st_blksize` line settles the question either way, and is worth
having even if the fix does nothing.

## 577 KB/s was not the bounce path either (0103, superseded above)

0102 predicted two outcomes and got the one that rules out the cheap fix:
the open is **99% I/O**, not CPU. `10462 KB in 18110 ms held of 18271 ms`.
minimp3's parsing is a rounding error.

But the throughput it measured said something louder:

| Window | KB/s |
| --- | --- |
| Track 1 open | 577 |
| Track 1 playback | 571 |
| Track 2 open | 577 |

Four windows, two files, and -- across the 0101/0102 boundary -- two bus
clocks, because `SDMMC_FREQ_HIGHSPEED` was accepted (`speed 40000 kHz`)
and moved throughput by about 4%. **A rate that does not change when the
bus clock doubles is not a bus rate.**

577 KB/s is 1127 sectors per second: 0.89 ms per 512-byte sector, against
about 26 us of data time for that sector at 40 MHz on four lines. The
other 0.86 ms was per-transaction overhead, and the transaction count was
being set by ESP-IDF's `sdmmc_read_sectors()` falling back to reading a
sector at a time whenever `esp_ptr_dma_capable()` says no to the
destination. It does not warn when it does this.

Every large buffer here is PSRAM -- `decoder.c`'s input window,
minimp3's 128 KB IO buffer through `malloc()`
with `SPIRAM_MALLOC_ALWAYSINTERNAL` at its 16 KB default -- so every read
in the program took that path.

`storage_io.c` now stages through one internal, 64-byte-aligned,
DMA-capable buffer of `STORAGE_IO_CHUNK` and memcpy's out. One buffer,
shared, safe because the lease already serialises readers. A destination
that is already DMA-capable and 4-byte aligned skips it.

**This is the second thing 0100 paid for.** Funnelling every read through
one function is what makes this a change in one place rather than six.

`bounced` in the report is the evidence: equal to `reads` means every
destination in that window was PSRAM. If the throughput does not move and
`bounced` is high, the hypothesis is wrong -- FatFs is entitled to stage a
read through its own window buffer rather than passing our pointer to the
driver, and the pointer tested here is the one given to `fread()`. Being
wrong costs a memcpy.

Two consequences to expect if it is right, neither of them the open:

- **`worst hold` should fall**, and it is the floor on control latency.
  221 ms at 577 KB/s is a 128 KB playback read; the same read at a burst
  rate is a fraction of that, and the 229 ms seek delay goes with it.
- **Everything gets faster, not just MP3.** Covers, tags, the walk, the
  esp_audio_codec input window and any future gapless read sit behind the
  same ceiling.

The open itself stays roughly 99% I/O either way. It is a whole-file read
and the only cure for that is not doing it -- `MP3D_DO_NOT_SCAN` with the
index rebuilt from a background walk, which is 0104 and which this makes
cheaper rather than replacing.

### The phase denominator was measuring the wrong window

0102's first track read `14798 ms held of 20979 ms (70%)`. The window runs
from the previous report, which for the first track is boot, so it
contained six seconds of somebody reading the chooser. Held over `open_ms`
was 99.2%, matching every later track. `play_file()` now resets the phase
at the open. A denominator that silently includes idle time gets the one
number this was built to produce wrong in the one case nobody checks by
eye.

Known rough edge: `covertag.c` is classed `PREFETCH` for every caller,
including the decode loop's own `load_tags()` at a track change. That read
should be `PLAYBACK`. It is a few KB and nothing below it can starve it,
so it is a wart rather than a bug; fixing it means passing a class into
the `covertag_*` entry points rather than fixing it inside `read_at()`.

One lease covers the card and the USB port together. That is wrong in
principle -- a cover read from USB does not contend with a decode from SD
-- and right in practice while nothing plays from one and reads the other.
When that stops being true it grows a lease per `storage_id_t`.

## The cyan flash was xStreamBufferReset()

For most of this project's life the panel showed a single cyan frame on
nearly every seek and at every track boundary. It was assumed to be a
DPI/PSRAM bandwidth problem -- the DSI bridge failing to fetch a line out
of PSRAM in time -- and six patches were written against that assumption
before anybody measured it.

It was `xStreamBufferReset()`. Called on a stream buffer that
`i2s_writer_task` may be parked inside `xStreamBufferReceive()` on.
FreeRTOS says a reset fails outright if a task is blocked on either side;
a writer released against indices that moved beneath it is not a defined
state for the highest-priority task in the program to be in. The last
link to a corrupted DSI frame is not proven and probably needs a scope,
but the localisation is not in doubt.

**The reset was enough even when the buffer was empty.** The boundary
site was made conditional on the ring actually holding something, the log
says it never did, and the flash went away.

### What was wrong with how it was chased

Every patch from 0403 to 0500 throttled something on the theory that the
DPI was being starved:

| Patch | Did | Result |
| --- | --- | --- |
| 0403 | paced the tail decode-ahead to 3x real time | no change |
| 0404 | split large blits into 240-row bands | no change |
| 0408 | 16 KB reads with a 2 ms pause; 8.6 -> 3 MB/s | no change, reverted |
| 0410 | lane rate 965 -> 700 Mbps | no change |
| 0411 | sliced `fread()` under the stdio buffer | no change |
| 0500 | paced the post-seek ring refill | no change |

Six "no change" results in a row, each recorded and none acted on as
evidence. The theory survived all of them because nothing had ever
tested whether the mechanism was operating at all.

**0501 tested it.** `cmake/dpi_instrument.cmake` vendors a copy of IDF's
`esp_lcd` with the DSI bridge underrun ISR's `ESP_DRAM_LOGE` replaced by
an increment of `g_tab5_dpi_underruns`, which `ui_task` reports once a
second when it moves. The counter has never moved. There were no
underruns, so there was no bandwidth problem, so all six patches were
aimed at a mechanism that was not running.

### The bisection that found it

Once bandwidth was out, the flash was localised by removing one thing at
a time from the seek commit and counting:

- `SEEK_NOOP` -- service the seek but never call `decoder_seek_sec()` or
  reset the ring. Fewer flashes, which was an impression and not a count,
  and was the weakest link in the chain for two patches.
- 0505 -- the INA226 at 1200 samples per 230 ms across a commit. Sag
  15-21 mV with the minimum landing anywhere in the window. Not a supply
  transient.
- `SEEK_KEEP_RING` -- seek the decoder, leave the ring alone. Nineteen
  seeks, no flash. The same build flashed at the next boundary, where a
  different path resets a ring.

That is the answer, bracketed from both sides: 0500 had already excluded
what happens *after* the reset, and `SEEK_KEEP_RING` excluded everything
else in the commit.

### The rule

**Do not call `xStreamBufferReset()` on a ring another task reads.** The
discard happens in the task that owns the read side: the decode loop sets
`s_pcm_flush`, and `i2s_writer_task` drains both rings with a zero
timeout at the top of its next pass. Draining from the decode loop
instead would be two readers on a structure that supports one, which is
worse than the reset it replaces.

One reset survives, at the boundary ring switch. It clears the ring about
to become `s_ring_fill`, which the writer is by construction not reading,
it is conditional on that ring being non-empty, and it logs if it ever
fires. If the drain it depends on is ever removed, that assumption goes
with it.

### What this cost, and the lesson

Five wrong hypotheses were published to the log before the right one:
ring memsets, `ui_draw()` contention, the post-seek refill burst, an
amplifier current spike, and a pack-voltage sag. Three of those were
derived by reading the source and looked convincing in the comment that
accompanied them.

The pattern in all five: **the code kept looking guilty and kept turning
out innocent, and each acquittal was treated as narrowing rather than as
evidence the frame was wrong.** Six "no change" results should have
retired the bandwidth theory long before a counter did.

Two process notes worth keeping:

- **Instrument the assumption before patching around it.** The notes
  flagged the missing underrun measurement as "the single highest-value
  next step" and then went five more patches without taking it.
- **A count, not an impression.** "Fewer flashes" from `SEEK_NOOP` was
  load-bearing for two patches and was never a number. `SEEK_KEEP_RING`
  was run as nineteen deliberate seeks and settled it immediately.

### The diagnostics it left behind

All default off and all one-build, in `player_diag.h` and at the top of
`player.c`:

| Flag | Disables | Answers |
| --- | --- | --- |
| `SEEK_NOOP` | `decoder_seek_sec()` and the ring drop | is the flash in the seek at all |
| `SEEK_KEEP_RING` | the ring drop only | seek vs. ring |
| `BOUNDARY_NO_INDEX` | minimp3's up-front index build | is the boundary's whole-file read to blame |
| `TRACE_DUMP_SAMPLES` | (enables) the per-sample pack dump | the shape of the rail |

`BOUNDARY_NO_INDEX` was never needed -- the index build was exonerated
without being switched off -- and it is kept because it is the cheapest
way to ask that question if a boundary problem ever comes back.

The DSI underrun counter is worth keeping longest. It is the only thing
that can distinguish a real bandwidth problem from another six patches of
assuming one.

**`TRACE_DUMP_SAMPLES` ate the log the first time it ran.** Seventy-five
warning lines in a few milliseconds against a 4 KB console buffer that
discards rather than blocks -- the failure documented at length in
`sdkconfig.defaults` and in "The console drops lines", happening again to
the person who wrote both. Four seeks logged `button: seek` with no
`seek to Ns` after it.

## Anything describing the audio belongs to the moment it is heard

The decode loop runs a ring ahead of the speaker. At a track boundary
that is twenty seconds. So there are two different "now" in this program
and every piece of state has to pick one:

- **Shaping the audio** -- `rg_scale`, the sample rate, the decoder's
  position. These belong to the moment the audio is *made*, because they
  are applied to samples the loop is producing right now.
- **Describing the audio** -- the title, the album, the envelope, the
  length, the seekability, the ReplayGain indicator, the chooser's
  playing-row marker. These belong to the moment the audio is *heard*,
  and are held in locals until `VISUALS_GATE()` releases them at the
  handoff.

`rg_scale` and `s_rg_gain_db` are the same number and land in different
categories. Deferring the gain would play the first twenty seconds of
every track at the wrong level; publishing the indicator early puts a
number on screen for audio nobody can hear yet.

Five things now go through the gate. Each was added after being noticed
separately on screen, which is the argument for a sixth being noticed the
same way rather than prevented:

| State | Was wrong how |
| --- | --- |
| title, album, artist | previous track's name over the new track |
| envelope | next track's waveform before the decoder opened the file |
| length, seekability, `s_stats_valid` | bar filling on the old track under the new name |
| ReplayGain indicator | gain changed 20 s early; then the outgoing one vanished 20 s early |
| chooser playing marker | accent moved to the next row while the old song played |

The mirror-image rule matters as much: **`play_file()` returning is not
the track ending.** It returns when the *decode* ends. Anything cleared
on the way out needs `if (!tail_playing())` or it goes off the screen a
ring early -- which is how the ReplayGain mark came to disappear twenty
seconds before the track it described. `s_pos_sec` already had that
guard; the gain and the marker did not.

The end of a folder is the one case with no handoff to republish
anything, so the clears live after the loop that waits for the tail to
play out.

### The chooser is told, not asked

`browser_set_playing()` is called from the gate. `browser.c` used to work
the marker out from `playlist_current()`, which is where the *decoder*
is, and the answer is not derivable on that side -- so the player
publishes it and the browser holds it. Setting it dirties the list,
because every other cause of a marker move is a press and this one
arrives from another task with nothing to ride on.

### And nothing draws over the chooser

`load_track_visuals()`, `do_art()` and `show_format_card()` all blit
straight to the panel, bypassing the `ui_draw()` that `media_task`'s
browser branch skips. All three check `browser_is_open()` and set
`s_repaint_art` instead. `show_format_card()` checks inside its wait loop
as well, because that wait is seconds long on a Xing-less MP3 and the
chooser can open partway through it.

## Nothing on screen may outlive the track it describes

A track change is decided in `track_change_begin()`, and everything that
was true about the previous track has to stop being displayed *there* --
not when its replacement is computed. The two are separated by
`decoder_open()`, which on a Xing-less MP3 is a full scan of the file:
twelve seconds on a USB drive.

Three things were being retired late and all three read as the player
having ignored the press:

- **The clock and the seek bar.** `s_pos_sec` / `s_len_sec` /
  `s_can_seek` were set just before the decode loop's first iteration,
  so the bar kept filling and the clock kept counting the *old* track
  under the *new* track's name. They are cleared at the decision now, and
  `s_stats_valid` is what says whether they mean anything. While it is
  false, `ui_draw()` draws dashed clocks and a bare groove -- unknown,
  rather than a confident wrong number.
- **The title.** The filename used to be installed immediately as a
  placeholder and replaced when the tag arrived, which flashes
  `04 - track04.mp3` on every change and is indistinguishable from the
  final answer on a file that genuinely has no tag. `load_tags()` runs
  before `decoder_open()` -- it is a couple of `fread()`s, or a cache hit
  -- and the filename is only reached as a fallback. An empty title row
  for a few milliseconds is honest; a filename for them is not.
- **The envelope.** Installed from the cache at the same point, so a
  prefetched track's waveform is on screen before the decoder has opened
  the file.

If something new is added to the screen, it gets cleared in
`track_change_begin()`, not wherever its replacement is computed.

## A file with no cover says what it is

`do_art()` draws a format card -- container, rate, channels, bitrate,
size -- through `ui_show_art_info()` when there is no picture. 720x720 of
black is what a cover that has not arrived yet looks like, so the two
states the player most needs to distinguish were drawn identically and
the honest one looked broken.

The decoder is the source for everything but the container and the size,
and `media_task` cannot ask it anything -- the `decoder_t` belongs to the
decode loop. So the decode loop publishes `s_fmt_*` on the first block,
the same publish-a-value rule as `s_ring_pct`, and the card waits briefly
for it.

"No cover" is cached as a bool. Without the negative, a file with no
picture is indistinguishable from one not yet read, and every return to
it re-reads the tag to learn the same nothing. Only `ESP_ERR_NOT_FOUND`
and `ESP_ERR_NOT_SUPPORTED` are cached that way: an allocation failure is
not a statement about the file.

## Prefetch is the whole track, not just the cover

`prefetch_next()` fetches tags, cover and envelope for the next track, in
that order, each stage re-checking the ring gate. They are not one
operation -- tags cost a few KB and the walk costs the whole file.

The prefetch walk was abortable through `s_prefetch_abort`, threaded
into the scan's polling, because a gate checked once at the start is fine
for a bounded 120 KB read and is not fine for a 60 MB one that would
otherwise keep running for seconds after the track it was for stopped
being next. The walk is gone and nothing that long runs on the prefetch
path any more; the remaining prefetch work is tags and the cover, both
bounded. `s_prefetch_abort` is separate from `s_scan_abort` because
they mean different things and are cleared at different moments.

Two consequences that are easy to undo by accident:

- `media_task` **skips** the walk stage when the envelope is already
  drawn; it must not `continue`, which it used to. Prefetch is the next
  track's business, and continuing meant the better the cache did, the
  less prefetching happened.
- The art settle delay is **skipped on a cache hit**. `media_settle()`
  exists to keep a second reader off the device; a cache hit is not a
  reader, and making it wait 700 ms throws away most of what the
  prefetch bought.

## The media cache hands out borrowed pointers

`mediacache.c` had no lock, and that was only safe because every caller
was `media_task`. The second caller has now appeared: the decode loop
reads tags and the envelope in `track_change_begin()`, because the entire
value of prefetching them is that they are on screen before anything slow
has run.

So there is a mutex, and the contract is split:

- **Copy-out, safe from any task:** `mediacache_tags()`,
  `mediacache_walk_copy()`, and the pin calls, which touch only flags.
- **`media_task` only:** `mediacache_art()`, `mediacache_walk()`, every
  `mediacache_put_*()`, `mediacache_clear()` -- they borrow past the
  lock, or they evict.

The rule behind the split is the old one: a borrowed pointer is bounded
by the next eviction, and only the borrower may evict. **The decode loop
therefore never stores anything.** It reads the tags and `media_task`
caches them a moment later, which costs one small read per track and
keeps eviction in one place. A lock alone would not have been enough --
a `put_*()` from the decode loop can evict the very blob `media_task` is
blitting.

`s_walk` is `media_task`'s scan buffer and the decode loop has its own,
`s_walk_pending`, for the same reason.

Ownership rules that are load-bearing:

- `mediacache_put_art()` **takes ownership**, including on the path where
  every slot is pinned and it cannot store the blob -- it frees it. A
  caller that also frees is a double free.
- `do_art()` tracks `owned`: a cache hit is borrowed and must not be
  freed, a fresh read is ours and goes into the cache rather than the
  bin. Even on the "track changed while reading" path, because the track
  it belongs to is very likely the one being returned to.
- `mediacache_init()` releases before it memsets. Calling it twice
  otherwise abandons up to a third of a megabyte.
- Pins are reassigned wholesale on each track change (unpin-all, then pin
  the outgoing track) rather than tracked per transition. Three slots,
  two pins, one for prefetch.

Tested host-side under ASan with the leak checker: ownership, LRU
eviction, pin protection, the all-pinned path, and the play/prefetch/
skip/back sequence.

## One press is one action

**Gating the input source is not enough. The iteration that changes
which screen is up must not also dispatch input to the new one.**

`ui_task` takes one touch sample at the top of each loop and hands it to
whichever screen is up further down. `touch_swallow()` gates
`touch_get()`, so it has no effect on a sample that has already been
copied into a local -- which is exactly the case on the iteration that
opens the chooser. The fix is that the opening branch draws and
`continue`s. The closing branch always did, which is why only the open
direction ever showed the fault.

If a screen transition is ever added elsewhere in that loop, it needs
both: `touch_swallow()` for the presses that follow, and `continue` for
the sample already in hand.

`touch_swallow()` exists because a tap that changes which screen is up
would otherwise be read twice: once by the screen that was up, and again
by the screen that just opened, whose edge detector starts out believing
nothing was down while the finger is still on the glass.

Call it on **both** sides of every screen transition, not just the one
you noticed. Opening the chooser without it played the eleventh track in
the directory, because the folder icon is at y=1100 and the chooser puts
list row 10 there. Closing it without it delivers the same press to the
transport bar underneath.

The swallow lifts on two conditions, both required: the finger has
lifted, **and** `TOUCH_SETTLE_MS` (250 ms) has passed. They cover
different failures. Waiting for the release stops the press that caused
the transition being read again by the screen it opened. Waiting out the
window stops the panel's own drop-and-reacquire -- a finger rolling
slightly makes the GT911 lose a point for one poll -- from becoming a
fresh tap on that screen milliseconds later. Either alone leaves a real
way to select something nobody aimed at.

The settle comparison uses signed tick difference, not `now < until`.
The tick counter wraps every 49 days at 1 kHz, and the naive form gets
it wrong exactly once per wrap, for 250 ms, on a device people leave
running. This was checked exhaustively against
`(int32_t)(now - until) >= 0` across the full difference space.

`TOUCH_SETTLE_MS` is the only number involved: raise it if the chooser
still feels like it selects on opening, lower it if paging through a long
directory feels sticky. The swallow is armed only on screen transitions,
so it never delays an ordinary tap.

## Cover art parsers are fuzz-tested; keep them that way

`covertag.c` parses four container formats from bytes that came off an SD
card, so every length in it is attacker-controlled in the only sense that
matters: a corrupt file should not be able to crash the player.

The parsers were validated with synthetic files per format plus 1200
mutated cases (truncation, byte flips, length fields overwritten with
`0xFFFFFFFF`) under ASan and UBSan, with targeted cases for each known
trap. If you change a parser, regenerate that corpus and rerun it rather
than eyeballing the bounds.

Rules that are load-bearing:

- **Cap before allocating.** `COVERTAG_MAX_IMAGE` is checked against the
  length from the file *before* `malloc()`, never after.
- **Check each length against what is left**, not against the total.
- **`found` counts fields actually filled**, not blocks encountered.
  Returning `ESP_OK` for an empty comment block tells `player.c` the tags
  are good and suppresses the filename fallback, leaving the title blank.
- **Trust magic bytes over declared types.** Taggers write the MP4 `covr`
  type indicator as JPEG for PNG data often enough that the indicator is
  a hint and `albumart_is_supported_image()` is the answer.

## The battery percentage is a guess and says so

`battery.c` reads the INA226 (U31, 0x41 -- A1 to GND, A0 to SOC_3.3V) on
the shared I2C bus: bus voltage, shunt voltage, two registers. The
**voltage is measured; the percentage is not** -- it is that voltage
through a piecewise curve, which sags under load and recovers when the
amplifier is muted by a headphone plug. Coulomb counting would fix that and needs a charge reference this
board does not give us.

So the reading is averaged in the part (16 samples, which is where audio
-rate ripple should be rejected), smoothed 1/8 in software, and rounded
to 5. That is not accuracy, it is a refusal to display precision that is
not there: a gauge stepping 63, 61, 64 while nothing happens is worse
than one that sits at 60, because the movement is the part people
believe.

Three things that will look like bugs and are not:

- **-1 draws an empty outline and no digits.** No gauge, or no reading
  yet. Drawing 0% would be a claim, and the wrong one.
- **The pack is 2S.** NP-F550, 7.4 V nominal: full is about 8.2 V and
  the board gives up around 6.0 V. A single-cell curve here does not read
  low, it pins at 100% for the whole discharge, because 7 V is off the
  top of it -- which looks like a working gauge on a full battery and
  goes on looking like one.
- **Positive shunt current is DISCHARGING** on this board, so
  `BATTERY_CHARGE_SIGN` is -1. R39 (5 mohm) has IN+ on the pack side.
  There is a threshold rather than a bare sign test because the reading
  dithers around zero at rest.
- **An absent gauge is not fatal.** `battery_init()` probes before
  configuring and returns `ESP_ERR_NOT_FOUND`; `app_main()` does not
  `ESP_ERROR_CHECK` it. If it reads a constant, doubt
  `BATTERY_INA226_ADDR` first.

## Missing glyphs are boxes, deliberately

Anything outside the subset draws a hollow notdef box, including five
Latin-1 characters Ark only draws fullwidth: `© ® ¼ ½ ¾`. A box is used
rather than `?` because `?` reads as a character the file actually
contained. `U+00A0` draws as a space and `U+00AD` is skipped; both are
handled in `gfx.c`, not baked into the table, because they are rendering
behaviour rather than glyphs.

## The 0800 series: the seek corpus, and what it found

0800-0810 started as documentation for `test_audio_files/` and turned
into six real fixes, every one of them found by a file that had never
been played rather than by reading the code. That is the point of the
folder and it is the reason this section exists.

### What the corpus is now

Twenty-one files, one minute of the same landmark audio, plus
`build/encode.sh`, which is the commands that made them. Before 0800
those commands existed only as the sentence "generated with ffmpeg
6.1.1" and could not be re-run. Every file rebuilds byte-identical
except the two Ogg ones, which carry a random serial, file 12, whose
cover image was not kept, and file 15, which needs an encoder ffmpeg
does not ship.

### The findings, shortest first

- **A chained Ogg is one stream (0802).** `oggseek.c` read the last
  granule from a 64 KB window at the end of the FILE, and in a chained
  file every page there belongs to the second stream. `last_granule`
  fell out as 0, which is not "no clamp" but a clamp that never fires:
  a drag past the end set a target no page could reach, so nothing was
  ever recorded as best and the seek returned the first audio page.
  Dragging to the right of a chained file restarted the track.
  `duration.c` had the same window and did not check the serial at all.
- **24-bit folds, 32-bit does not (0803).** The samples are already in
  a buffer of ours one call before they belong to anything else, so
  rounding them to 16 there costs a pass over the frame and changes
  nothing downstream. 32 stays refused because
  `esp_audio_simple_dec_info_t` reports a bit count and no way to tell
  an integer stream from a float one, and folding float as integer is
  full-scale noise into headphones.
- **A failed decode is not the end of a track (0804).** `if (n <= 0)
  break` made an error indistinguishable from end of file, so a stream
  that died on its first frame counted as a complete uninterrupted
  play: -46.16 LUFS off zero gated blocks, and a one-column envelope
  for a sixty second track, written to a sidecar and loaded back on the
  next run to be drawn as a full-height block. -46 LUFS asks for about
  +32 dB. Everything written to the card now hangs off ENDED AND
  DECODED rather than off `why`, which still answers only what to play
  next.
- **The ADTS table is walked, not listened to (0805).** Every ADTS
  header states its own frame length, so the table can be chained out
  of the headers without decoding anything -- 979 KB in 180 ms on the
  board, at BACKGROUND, behind the music. It replaces a design that
  cost one play to learn the table and a second to use it, and that a
  single drag would throw away. The recording path stays armed
  underneath as a fallback.
- **ALAC seeks (0808).** See the MP4 section above. The note that ruled
  `_ALAC` out was true of a file read as a stream and false of one read
  through a sample table.
- **A landing is reported in hundredths (0809).** Every seek report
  truncated: a landing at 35.94 s printed as 35, and `player.c`
  re-anchors its position counter from that number, so the truncation
  was up to a second of error handed to a clock that never corrects
  itself. Measured over the corpus, MP4 lands within 0.09 s, TS within
  0.13, Ogg within 0.99. The mechanisms had always been that close.

### Four lessons that cost something

**Verify against the decoder that will run it.** 0801 built a CBR ADTS
file by padding every frame to a constant length and checked it by
decoding with ffmpeg, byte for byte, against the source. That check
passed and meant nothing: the padding is zero bytes after the raw data
block's terminator, ffmpeg stops at the terminator, and Espressif's
decoder reads on to the declared frame length and finds `ID_SCE` -- a
channel element built out of zeros. One block, `error:30`.

**A test whose wrong answer equals its right one is not a test.** File
18 was two 30-second Ogg streams. The duration bug read the second
stream's granule, which was also 30, so the file certified the bug as
passing. Rebuilt as 20 + 40 the two answers became 20 and 40 and only
one of them could be printed.

**A file built to a probe's assumptions proves nothing about the
probe.** The padded file passed `cbrseek.c`'s ADTS branch because its
frames were identical by construction. A real fdk-aac CBR file spread
3.42% across the 4 KB sample windows and was refused -- so that branch
would have turned away every genuine CBR AAC file ever handed to it,
and looked correct while doing it. The window is 16 KB now, where the
same file spreads 0.28%, because a CBR encoder holds a constant AVERAGE
rate and borrows bits between frames.

**A change of units that leaves the name alone is a change every caller
compiles cleanly against and gets wrong.** 0809 renamed
`decoder_seek_sec_at()` to `decoder_seek_sec_at_cs()` and
`mp4_seek_sec()` to `mp4_seek_cs()` for that reason alone.

**A comment is stale from the moment the code under it moves.** 0812
found `player.c` still telling the reader that seek "is not implemented
yet -- see README", four patches after 0808 removed the last format that
could not do it, and pointing at a README that had never described the
mechanisms it was deferring to. It had also drifted three declarations
away from `s_seek_pct`, so nothing about its position said what it
described. 0811 struck three finished entries out of a list; this is the
same failure in a place no list looks -- and it is worse there, because a
list is read as a claim about the past while a comment is read as a claim
about the line beneath it.

### Still open, and deliberately

- ~~**`mp4_probe()` fails silently.**~~ Closed by 0813. Every refusal
  sets a reason string and one line at the bottom prints it. The old
  behaviour was a dozen bare `goto out` and one logging path, so file
  16 (fragmented MP4) printed `0 samples is outside what is held here`
  by luck rather than design -- and that was not even the reason, since
  a fragmented file keeps its tables in `moof` boxes and the empty
  `stsz` is downstream of that. It now says `no usable stco or co64
  box`, which is the check that actually refused it.
- ~~**The MP3 sidecar index never re-harvests at a finer spacing**, where
  the in-memory one does.~~ Closed by 0711 and struck by 1002.
  `decoder_index_extract()` makes exactly this exception: an installed
  table is normally not written back, but when minimp3 has since built
  its own finer index on top of it, the finer one wins. The test is
  `d->indexed && d->ex.index.num_frames <= d->index_count`, which is one
  comparison and needs no flag. A sidecar written at the old ten-second
  spacing upgrades itself to `REPLAYGAIN_INDEX_SPACING_SEC` (2 s) the
  first time anybody drags in that track.
- **AMR has neither a test file nor a walk.** No stock ffmpeg can
  encode it, and a synthetic file with valid headers and junk payload
  would test the probe and be useless to listen to, which is the wrong
  trade for a corpus judged by ear.
- ~~**`ESP_AUDIO_SIMPLE_DEC_TYPE_ALAC` logs `Not find default parser`**
  at every open.~~ Closed by 0907 and struck by 1002. The diagnosis in
  this entry was right -- `1128352833` is `ALAC` read the other way up,
  and with `use_frame_dec` the sample table does the framing -- and 0907
  acted on it: `SDEC_TAG`'s threshold is raised to `ESP_LOG_ERROR` across
  both ALAC opens in `decoder.c` and restored before the M4A fallback
  open below them, which has every reason to be heard. The entry
  outlived the fix by the rest of the 0900 series.
