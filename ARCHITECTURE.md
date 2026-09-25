# ARCHITECTURE.md

What this code is, and why it got that way.

Split out of `CLAUDE.md`, which is now only the rules for writing a
patch against it. Nothing here was rewritten in the split: the sections
are in the order they were written, which is roughly the order they were
learned, and the numbered series near the end are a record of what was
tried rather than a description of what is. Where one of them contradicts
an earlier section, the later one is what happened.

**This file is reference, not instruction.** The things that constrain
how a change is made -- patch numbering, authorship, stack sizes, the
one-patch-one-number rule -- are in `CLAUDE.md`, and a session that reads
only this file will write patches that get sent back.

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

Not `idf.py fullclean` alone, which this file used to offer as an
alternative: in IDF 5.5 it deletes the contents of build/ and leaves
sdkconfig untouched, so every stale value survives it. Verify with
`idf.py menuconfig` under
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

### A fade in the recording, and the silence after it

The crossfade ramps the outgoing track down whatever the track is
doing, so a song mastered with its own fade was faded twice. Before
anything can decline to do that, something has to know which tracks
fade. The sidecar's `fade` section is that record.

It comes from the loudness pass. `loudness_t` keeps the last 60 s of
400 ms block loudness in a ring, gated or not, and `loudness_fade()`
reads it when the track ends:

- **The end of the audio** is the last block within 40 LU of the
  track's integrated loudness (floor -70 LUFS). Everything after it is
  recorded silence, and `audio_end_ms` is stored whether or not there
  is a fade. Relative, so a quiet room tone after a loud master counts
  as the silence it is at normalised volume.
- **The start of a fade** is found by walking back from the end for as
  long as the level keeps climbing. That finds a linear-amplitude fade,
  whose first seconds barely move, and lets a quiet outro fade from the
  outro.
- **A fade** falls at least 10 LU over at least 1.5 s. A held chord
  ringing out qualifies, deliberately: for anything deciding whether to
  ramp a track down it is the same problem.

All of it is relative to the track's own level, so ReplayGain does not
move the answer -- which is why a track already measured is examined on
its next complete play at its normal gain, not re-measured at unity.
`fade_measuring` is its own flag for that reason. A seek abandons it.

Not a `REPLAYGAIN_FORMAT_VERSION` bump: a bump discards every line on
the card, and an absent section already means "nobody looked". The
section carries `LOUDNESS_FADE_VERSION` instead.

Limits worth knowing: start times land about half a second early on
clean fades and late on fades that start very slowly (a quarter-sine
fade reads as starting where it steepens); a fade starting more than a
minute before the end is reported as none; more than a minute of
trailing silence reads as unknown, not as silence. Host-tested in
`texttest/fadetest.c` against the real `loudness.c`, on noise, which has
no bars or reverb -- the `fade:` and `silence after the audio:` lines on
a real library are the check that matters. **Not flashed.**

### What a boundary does with that ending

`main/tailplan.h` turns the `fade` section into what the boundary does,
as pure functions so `texttest/tailplantest.c` can test the rules
themselves. `player.c` only applies the answers.

- **Recorded silence is cut to 3 s.** The decode stops at
  `audio_end_ms + TAIL_SILENCE_KEEP_MS` and the track ends there as an
  ordinary end, so the crossfade countdown, the dip and the plain
  handoff all see the shorter ending without knowing why. Never on a
  pass that is measuring, and a cut play does not file a length or a
  seek table, since both would describe a shortened file.
- **Perceived silence (1 s or more, no fade) gets no fade in.** The
  writer disarms the overlap and the next track starts at full level
  after the gap. At a rate change the dip is skipped both ways.
- **A recorded fade is not faded again.** The outgoing track plays at
  unity -- the level match never attenuates it mid-fade -- while the
  incoming one fades in over half the configured crossfade or the fade's
  own length, whichever is shorter. The overlap is placed to finish where
  the outgoing audio does, so the silence kept after the fade is dropped
  by the overlap's end rather than heard. At a rate change the dip has
  no down half and its up half is shortened the same way.
- **Anything else is the crossfade as before**, including every track
  not yet examined.

A fade followed by silence is a fade. 1 s and 3 s are estimates, not
measurements.

**Seen on hardware (v0.3.0-78):** the Bach track ended `recorded
silence cut to 3000 ms`, and the next boundary logged `crossfade: 2600
ms fade-in over a recorded fade, trim out 100% in 100%` -- the outgoing
track at unity, the fade-in at half the configured crossfade. Whether
it sounds right was not reported. Known cost: a cut track ends before its seek bar reaches
the end. Not compiled against ESP-IDF here. **Not flashed.**

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

### The hardware has no scaler, and that claim was true (1101)

`albumart.c` has asserted since it was written that the P4's JPEG
decoder produces the picture at full size or not at all. **Checked, and
it is right.** `jpeg_decode_cfg_t` carries `output_format`, `rgb_order`
and `conv_std` -- no scale, no sub-rectangle, no strip -- and the struct
is identical across the ESP-IDF P4 driver documentation for v5.3, v5.3.1,
v6.0.2 and v6.1. It has never had a scale field.

**That is the first load-bearing claim this session checked that turned
out true**, after the ring size, the seek bullet, the 16-bit limit and
`peaking?` all turned out stale. Worth recording as plainly as the
corrections: the file is not uniformly wrong, and knowing which parts
held up is what makes the rest worth reading.

So the 3000 px cover needed a second decoder, and TJpgDec via
`espressif/esp_jpeg` is it.

**A fallback and nothing else.** It is reached only on the branch that
1010 instrumented -- the one where `heap_caps_get_largest_free_block()`
is already short and the code used to print a number and give up. Every
cover that fits keeps the hardware path and its milliseconds.

| | Hardware | TJpgDec at 1/4 |
| --- | --- | --- |
| 3000x3000 buffer | 17,672 KB | **1,098 KB** |
| Speed | ~550 ms | seconds |

The scale is the gentlest reduction that still leaves at least the
panel's worth of pixels in both directions, so the 16.16 fit downstream
is still reducing and nothing visible is given away: 3000 px against a
720 px box picks 1/4 for 750 px. A 6000 px cover picks 1/8 for the same
750. If a scale will not allocate it keeps halving and, at the end,
accepts an upscale rather than no cover -- which only arises for covers
small enough that the hardware path would not have failed anyway.

**The cost is why it is not the default.** TJpgDec Huffman-decodes every
MCU whatever the output scale, so scaling saves memory and not time: a
nine-megapixel cover is seconds of CPU against the hardware's hundreds
of milliseconds. It runs on the media task, which is where 1005's
watchdog fired, and `esp_jpeg_decode()` is blocking. **That is the thing
to watch on the first board run**, and if it trips, the answer is not to
make it faster but to stop doing it -- the format card was an acceptable
outcome before this patch and still is.

Three details that will matter to whoever touches this next:

- **The blit reads its dimensions from whichever path ran.** The
  hardware pads to 16-pixel boundaries so the stride is not the width;
  TJpgDec writes tightly so the stride *is* the width. Three variables,
  one `goto`, and the scaling loop below is untouched.
- **`swap_color_bytes` is 0**, against the README example, which sets it
  for LVGL. The shadow buffer, `gfx.c`'s `RGB()` and the DPI panel are
  all native little-endian RGB565 and agree with each other. If a cover
  comes back with reds and blues exchanged this is the one line to flip
  -- and it is the same fault the `rgb_order` note above describes, from
  the other side, so the symptom to look for is a gold cover rendering
  silver.
- **Not in ROM on this chip.** The ROM copy exists on ESP32, S3, C3, C6,
  C5 and C61; the P4 is not on that list, so this is roughly 5 KB of
  flash rather than free.

**Licensing.** TJpgDec is ChaN's, under its own permissive text, and the
notice has to travel with a redistribution -- the second such obligation
after the font's OFL. Recorded in the README's licensing section, which
also gained the MurmurHash2 line from 1011.

### The software decoder never had room to start (1104)

1101 shipped the TJpgDec fallback uncompiled. It compiled, it ran, and
every cover failed:

    E JPEG: esp_jpeg_decode(98): Error in preparing JPEG image! 3
    W tab5_art: software decode at 1/4 failed (ESP_FAIL)

**Not a memory shortage, and nothing to do with the fragmentation the
line above it reports.** `3` is TJpgDec's `JDR_MEM1` and it came from
`jd_prepare()`, before a single MCU was decoded. The 1098 KB output
buffer allocated fine; the code never reaches `esp_jpeg_decode()`
otherwise. What was short was TJpgDec's scratch pool, which esp_jpeg
sizes at compile time and which is 3100 bytes on this build.

**The arithmetic, which is the whole finding.** `jd_prepare()` takes from
one pool: `JD_SZBUF` for the input buffer, the quantiser tables at 64
`int32_t` each, the Huffman tables at `16 + np*2 + np`, a `workbuf` of
`n*64*2 + 64`, and an `mcubuf` of `(n + 2) * 64 * sizeof(jd_yuv_t)`.
That last type is `int16_t` when `JD_FASTDECODE >= 1` and `uint8_t` when
it is 0. For a 4:2:0 image, where `n` is 4:

    FASTDECODE 0:  3096  against a pool of 3100
    FASTDECODE 1:  3480  against a pool of 3100

**3100 is tuned for `JD_FASTDECODE=0` and clears it by four bytes.** The
comment beside it in `jpeg_decoder.c` -- "Independent on the size of the
image" -- is true and beside the point: it is independent of the
dimensions and dependent on the subsampling and on `JD_FASTDECODE`, and
it checks neither. This project builds `CONFIG_JD_FASTDECODE=1`.

So the pool is ours now: 8 KB of internal DRAM through
`cfg.advanced.working_buffer`, one buffer for the whole scale ladder,
freed on every exit. Not `CONFIG_JD_FASTDECODE=0`, which would also work
and would give up the 32-bit path on a decode 1101 already measured in
seconds on the task 1005's watchdog fired on. 8 KB for the length of one
decode against every cover being slower for ever.

**The shape of the bug is worth keeping.** It depends on chroma
subsampling, not on dimensions -- a 4:4:4 cover has `n = 1` and fits
either way, and a 200x200 4:2:0 cover fails identically with megabytes
free. It looked like a large-image problem on a memory-starved path and
was neither. Two logs' worth of "the 3000 px cover is too big" was the
wrong reading of both.

**Two config symbols 1101 depended on without saying so.**
`CONFIG_JD_USE_SCALE` must be set or `tjpgdcnf.h` defaults `JD_USE_SCALE`
to 0 and every scale but 1/1 returns `JDR_PAR` from `jd_decomp()` -- a
different failure at a later stage, and the scale is the entire point of
this path. `CONFIG_JD_FORMAT` may be either: 0 is RGB888 and esp_jpeg's
output callback converts to RGB565 for us, with `out_color_bytes` taken
from `cfg.out_format` rather than `JD_FORMAT`, so the two-bytes-per-pixel
arithmetic holds whichever way it is set. Both were checked against the
component source rather than assumed.

**And the log line gained its context.** `esp_jpeg_decode()` collapses
every `JDR_*` code into `ESP_FAIL`, so `failed (ESP_FAIL)` cost two board
runs to distinguish a work-pool shortage from a real allocation failure.
The component prints the raw code one line above at `E`; what it does not
print is any of the state that says which meaning applies. The scales and
both buffer sizes are in the line now, and the comment above it says what
each raw code means so the next reading does not need this round trip.

**Host-tested, not built, not flashed.** `texttest/pooltest.c` transcribes
`alloc_pool()`'s word rounding and `jd_prepare()`'s allocation sequence
and runs both pool sizes against both `FASTDECODE` settings: 3100 fails
at `FASTDECODE=1`, passes at 0 by four bytes, passes at 4:4:4, fails
independently of image size, and 8 KB clears the worst case of four
quantiser tables and 256-code Huffman tables with 3.5 KB spare. That
tests the explanation, not the board -- if the next log still shows `3`,
the pool is still short and `JPEG_SW_WORKBUF` goes up; `5` means
`CONFIG_JD_USE_SCALE`; `6`-`8` mean the cover is progressive, which
TJpgDec cannot decode at any pool size.

### tail-lost was telling the truth about the wrong thing (1116)

A clean album -- thirteen tracks, eleven crossfades, `done=11
tail-dry=11`, none of the failure lines -- with one line in it that lied:

    W tail on ring 1 abandoned (2057 KB unplayed): a shorter track ended
      before it drained

**That audio was not unplayed.** The retire fired because
`s_tail_pending` is still one slot and `Prelude`'s tail latched while
`Beautiful & Broken`'s was pending. But since 1115 the rings are a queue:
nothing reset ring 1, the writer stepped to it in turn, and it drained
normally. The `played out` twenty seconds after `crossfade done` is that
ring finishing.

So the counter had changed meaning underneath its own label. Before 1115
`tail-lost` meant audio destroyed; after it, at the latch site, it means
the slot was reused while the audio played on regardless. A line claiming
`2057 KB unplayed` on a boundary where nothing was lost sends the next
reader hunting a bug that is already fixed -- which is the specific
failure mode this file exists to prevent.

**Two outcomes, two counters, two levels.** `tail-lost` at `W` is the
reset path -- a pause, a seek, or a track change -- where the audio
really is discarded. `tail-slot` at `I` is the latch path, where the only
thing given up is the ability to report `played out` for the older of two
tails: one slot, two tails, the newer one wins it.

**What has not changed** is that both release the screen. A boundary that
retires either way still has to stop the display waiting for something
that is not coming, which is 1112's rule and applies to bookkeeping
exactly as much as to loss.

**Host-tested, not built, not flashed.** `texttest/tailtest.c` gained the
distinction rather than just the counter: a second tail takes the slot
with `lost == 0`, advancing onto a tail's ring discards with `lost == 1`,
and fifty consecutive short tracks produce takeovers but **no discarded
audio at all**. That last assertion is the one that would have caught the
mislabelling, and it is stated over the outcome rather than the count,
because the count per boundary is an implementation detail and the
absence of loss is not.

### The rings are a queue, not a pair (1115)

1114's wait fired and the stall survived, which finally showed the shape
of it:

    W tail on ring 0 abandoned (2065 KB unplayed)     track A
      ... track B latches its tail on ring 1 ...
    I waited 11920 ms for ring 0 to play out          track C
    W tail still pending ... play ring 0, fill ring 0, tail ring 1

**The writer never visits ring 1.** Its handoff was
`s_ring_play = s_ring_fill` -- a jump to the NEWEST ring rather than a
step to the next one. With two tracks in flight those are the same ring
and it never showed. With three they are not, and the ring in between is
skipped permanently, because nothing ever goes back for it.

Two changes, and they only work together.

**The handoff steps by one.** `s_ring_fill` only ever advances by one, so
stepping by one is what makes the two agree. There is a fallback for the
case a flush creates -- a seek empties rings without moving either index,
after which the next ring in sequence can legitimately be empty while the
fill ring holds the audio -- and it logs, because jumping is right there
and wrong everywhere else.

**And PCM_RINGS is 3.** This was argued against twice and both arguments
were wrong. 1113 said a third ring "does not restore the invariant".
1114 corrected that to "the wait restores it, the ring count decides how
often it fires". Both missed the real point: with three tracks in flight
the play order must be A, B, C while only two rings exist, and **there is
no assignment that satisfies it**. The third ring is not about frequency;
it is about the ordering being expressible at all.

A track shorter than `PCM_RING_BYTES` is the trigger, and twenty seconds
of ring against a twenty-second `Prelude` is not a corner case -- album
interludes are routinely that length.

**What it costs, and where it stops.** 3.5 MB more of PSRAM, rings going
from 7 MB to 10.5 MB, competing with 1104's software decode (1098 KB) and
1111's kept frame (2 MB cap) for a structural 16128 KB largest block.
FOUR tracks in flight -- two consecutive short ones -- would need a
fourth ring by exactly the same argument, and that is where this stops
being affordable. 1114's wait is the backstop for that case, which is
what it should have been introduced as rather than as the fix.

**One thing the change nearly broke silently.** The boot check was
`s_ring[0] && s_ring[1]`, which would have accepted a two-ring player the
moment `PCM_RINGS` became 3 -- a player short of a ring being exactly the
shape of bug this release has spent four patches on. It is a loop now.

**Host-tested, not built, not flashed.** `texttest/orderingtest.c` states
the property over the AUDIO: every byte written is eventually played, in
the order the tracks were decoded. It asserts two rings CANNOT express
three tracks, that three rings can and in order, that an album of
alternating short tracks holds across twenty of them, and that four in
flight fills every ring so the next decode must wait rather than
overwrite. The twenty-track case failed first time and the failure was
real -- it was demanding four-in-flight behaviour from three rings, which
is the limit above rather than a bug below it.

### Never reuse a ring that is still being heard (1114)

1113's guard fired and the stall survived, which located the fault
precisely:

    W tail on ring 0 abandoned (2057 KB unplayed)
    W incoming ring 0 held 2041 KB; resetting
    W tail still pending ... play ring 0, fill ring 0, tail ring 1

**play AND fill both 0.** The reset landed on the ring the writer was
reading -- the one case the comment at that site rules out as impossible.

It takes three tracks in flight and two rings. A track shorter than
`PCM_RING_BYTES` -- twenty seconds of audio against a twenty-second
`Prelude` -- ends while the PREVIOUS track's tail is still queued and
unplayed. The writer only advances when its own ring empties, so it never
reaches the short track's ring, and the track after that comes back
around to a ring that is still being read.

**1113 guarded the wrong thing, and this is the lesson worth keeping.**
"Not the tail's ring" is a PROXY for "not in use", and at that boundary
the proxy picked the wrong ring: it retired the tail the writer was about
to play and left the genuinely stranded one alone. The accounting came
out right -- `tail-lost=1`, screen released, which is why the display
stayed correct -- and the audio was still destroyed under the writer.

The invariant is simpler than the proxy: **never reuse a ring that still
holds unplayed audio.** With two rings and three tracks there is no legal
ring, so the only correct move is to wait.

**Waiting costs nothing audible.** The writer has up to a ring queued, so
the listener hears no gap; what is delayed is the decode of a track that
will not be heard for another twenty seconds. The bound is the time that
ring takes to drain.

It yields to the pause gate, because a paused writer drains nothing and
waiting on it would hang the loop that reads the controls -- including
the one that unpauses. It yields to a seek and to a track change, which
supersede this track entirely. The reset survives as the last resort for
exactly those exits, where the audio is being discarded on purpose, and
1113's retire still runs there so the tail's state goes with it.

**And a correction on the third ring.** 1113 said a third ring "does not
restore the invariant" and buys only a lower collision rate. That was
wrong in a way worth recording. The invariant is restored by the WAIT;
the ring count decides how often the wait fires. With three rings the
track after a short one takes a fresh ring, nothing waits and nothing
resets. They are not alternatives -- one is the correctness fix and the
other is the headroom that keeps it from mattering. `PCM_RINGS` is left
at 2 deliberately, so the `waited N ms for ring` line says whether 3.5 MB
is worth spending before it is spent.

**Host-tested, not built, not flashed.** `texttest/ringwaittest.c` states
the property over the AUDIO rather than over the tail flag, which is
where 1113 went wrong: no reset lands on a ring holding unplayed bytes
except on the exits that discard deliberately. It covers ordinary
boundaries never waiting, the short-middle-track case waiting exactly
once, pause and seek not blocking, and the bound holding against a writer
that never drains. The first run failed on the ordinary case and the
fault was in the model -- it never let the writer reach the second ring.

### One tail slot, two tails (1113)

`tail still pending after 60023 ms` survived 1112, and with no `partial=`
anywhere in the tallies -- so 1105's exit was never taken and **1112's
diagnosis of this symptom was wrong**. 1112 fixed a real bug, the frozen
screen after a cut-short overlap, and was credited with one it had
nothing to do with.

**The actual fault is that `s_tail_pending` and `s_tail_ring` are one
slot.** Two things can leave them naming a ring that will never report
empty:

- a second tail latching before the first has drained, which overwrites
  `s_tail_ring` and forgets the older ring entirely;
- the decode loop advancing onto a ring the tail still owns and
  resetting it, which empties the audio but leaves the flag set against a
  ring that is now the new track's.

Both need a track shorter than the ring depth. `PCM_RING_BYTES` is about
twenty seconds and `Prelude` is twenty seconds, which is why every
occurrence in every log is around that one file.

**The code had been reporting it for a while.** The reset site claims the
ring it advances onto is always already empty, and says outright that if
the log is not silent then the assumption was wrong and *that* is the
bug. `incoming ring N held X KB; resetting` fired three times in one
session and a stall followed each.

**Not a third ring.** That was the obvious answer and it does not restore
the invariant -- with two rings one short track collides, with three you
need two consecutive short ones, which albums with interludes have. It
buys a lower rate and keeps the same silent failure, which is the worst
combination: harder to reproduce, identical symptom. It also costs 3.5 MB
against a structural 16128 KB largest block that 1104's software decode
and 1111's kept frame are already competing for.

**Nor a crossfade threshold.** The stalled boundary had already been
refused: `no crossfade: the incoming track is only 20 s` fired at 936494
and the collision happened at 944973 regardless. The tail mechanism runs
on every boundary whether or not an overlap does, so no crossfade rule
can reach this. Tying 1108's threshold to the ring length would also be
wrong on its own terms -- that rule is musical, about a fade not
occupying more than half a track, and a 2 s fade on a 15 s track is
perfectly reasonable.

So `tail_retire()`, called from both sites. **The audio was already being
lost either way** -- the reset threw it away, the overwrite left it
unplayable. What was missing was anyone saying so, and the state going
with it. This makes an existing silent loss into an event, counted as
`tail-lost` and releasing the screen the way every other end of a tail
does. Without that release the boundary would join 1112's list of ways to
finish a handoff with the screen still on the previous track.

> **Superseded in part by 1115.** The sentence above -- "the audio was
> already being lost either way" -- stopped being true at the LATCH site
> once the rings became a queue. With a sequential handoff the older
> ring is still played in its turn, so a second tail latching now costs
> only the bookkeeping slot. The two cases are counted separately as
> `tail-lost` and `tail-slot`; see 1116.

**Host-tested, not built, not flashed.** `texttest/tailtest.c` models two
rings and drives fifty consecutive short tracks through them, asserting
that exactly one tail is ever pending and that every abandonment is
counted. The interesting failure was in the test rather than the code: it
lowered the release inside the latch, and the release belongs to
`track_change_begin()` -- the retire's raise is about the boundary being
abandoned, not the one after it.

### 1105 was worse than the thing it fixed (1112)

The board found it, and the symptom was a screen that stopped changing:
"Beautiful & Broken" stayed up across two subsequent tracks until a
manual selection cleared it.

**The cause was 1105's exit.** At 1318921 a crossfade started; at 1320698
it ended through the partial-frame branch, `14% through the overlap`. The
midpoint was never reached, so `s_visuals_released` was never raised, and
the exit did not raise it either -- so nothing was left that could change
the screen. It also moved `s_ring_play` to the fill ring while the tail
was still in the other one:

    W tail still pending after 60002 ms -- this should be impossible
      play ring 0 (3600384 B), fill ring 0 (3600384 B), tail ring 1 (3253308 B)

3.2 MB that nothing drains and `s_tail_pending` set for ever. Both stalls
in that log follow a `partial=` increment and nothing else does.

**So it falls through again.** 1105's reasoning was right -- falling
through does the ordinary single-ring receive with `s_xfade_active` still
true, writing at unity partway down a fade -- and its conclusion was
wrong. One chunk of unity-gain audio inside a fade is a worse-sounding
boundary. An orphaned tail and a frozen screen are broken ones. The
fall-through produced neither across five board sessions.

1107's counter stays, which is the only reason this was findable: the
`partial=1` then `partial=2` in the tallies is what tied two
sixty-second stalls to one branch.

**Every exit releases the screen now.** Only two of them did -- `done`,
because it runs past the midpoint, and the tail-dry check. That was
enough while nothing else ended an overlap early, and 1105 added
something that did. The rule is now that ending an overlap and releasing
the screen are the same event, so the question has one answer rather than
one per branch.

**And the length rule had a first-play hole.** "Unknown is not short" is
the right default, but a track with no sidecar yet reported 0, so 1108's
rule did not fire and the board crossfaded 12 s into a 20 s `Prelude`
that had never been played. The very next boundary, once the sidecar
existed, refused correctly with `the incoming track is only 20 s` -- the
same file failing and passing the same test on consecutive plays. The
decoder is already open at the arming decision and its index knows the
length, so it is asked when the sidecar has nothing. Had that fired at
1318921, neither stall would have happened.

**The honest summary of 1105 and 1106.** 1105 guarded a case nobody was
hitting, which was said at the time; when the case did start being hit,
the guard was the failure. 1106 was built to catch a stall that was not
happening, and then caught this one -- the watchdog's value has now twice
been something other than what it was aimed at.

**Host-tested, not built, not flashed.** `texttest/releasetest.c` sweeps
every exit rather than checking any one of them, because the fault was
the existence of an exception rather than the behaviour of a branch. It
also asserts the shape of the new partial-frame case: it does not end the
overlap, so it cannot end it without releasing.

### One decoded cover, kept (1111)

`mediacache.h` has said since it was written that caching the decode
would cost forty times the memory to save a delay nobody can perceive.
That was true while every cover went through the hardware codec in
single-digit milliseconds. **1104 overturned its own premise.** Routing
the 3000x3000 cover through TJpgDec made it work, and the board then
measured the same picture -- same 1871582 bytes, same `hash 04d36f37` --
decoding for 4.6 seconds at every track change AND at every
settings-panel close. Nine decodes in one session, nine identical
hashes, one of them necessary.

4.6 s is not a delay nobody can perceive, and it runs on media_task, so
the prefetch queues behind it: `prefetched tags` landed ~500 ms after
each decode finished rather than during the track.

So one frame is kept -- the last one drawn -- with the hash of the
bitstream it came from. **This is 1011b, and it only became possible by
accident.** 1011 wanted exactly this and could not have it, because the
hash existed only inside `albumart_draw()`; 1102 put it on the store
path as a side effect of sharing buffers, and 1104 made it one function
rather than two that had to agree.

**The hash is taken on the caller's bitstream, before an engine or a
buffer exists.** Hashing the DMA copy would mean allocating the input
buffer to discover it was not needed. The log line still reports it, so
a board log reads the same on both paths.

**The cap is the load-bearing part.** A full-size decode of a large cover
is megabytes -- 3000x3000 is 17 MB -- and the largest free PSRAM block is
a structural 16128 KB with the framebuffer across the middle of the heap.
Retaining a frame that size would guarantee the next large cover cannot
decode at all: a slow success turned into a permanent failure. Two
megabytes covers what is actually reached -- a 700x700 native decode is
~991 KB, TJpgDec's 1/4 of a 3000 px cover is 1098 KB -- and anything
larger is drawn and dropped.

**And the old frame goes before the new decode starts, not after.** Held
across it, it is a megabyte of the contiguous block the decode is about
to ask for, on the one path where that block is already 1544 KB short of
what the hardware wanted.

Two smaller rules, both of which the host test exists to hold:

- **Kept only if it was drawn.** A frame that failed to blit is not what
  is on the panel, and answering a later request with it would put up a
  picture nobody has seen instead of retrying.
- **Ownership moves.** `cover_retain()` takes the buffer and says so; the
  caller frees only what was refused. A frame that is both kept and freed
  is a use-after-free the board would show as a corrupted cover, much
  later, on an unrelated track.

**What this does not cover.** PNG covers go through `albumart_draw_png()`,
which streams through pngle and never holds a full bitmap, so there is no
frame to keep and no change here. A PNG cover still re-decodes on every
repaint. That is the right trade while pngle's whole point is not
materialising the image, but it means the fix is JPEG-only and the log
will show it.

**Host-tested, not built, not flashed.** `texttest/kepttest.c` under ASan
checks what makes the cache safe rather than that it works: repeated hits
never decode, a different hash never hits, an oversized frame is dropped
rather than kept, a failed blit is not answered with later, the release
is idempotent, and a hash of 0 does not collide with "nothing kept".

### Settings belong to the player, the resume track belongs to the card (1110)

The volume-follow overwrite, closed. It was left open through five
patches because it was a policy question rather than a defect, and the
policy is now: **settings are mirrored to every volume present; the
remembered track is per-volume.**

The split is the whole patch. Volume, ReplayGain and the two crossfade
settings describe the PLAYER -- someone set them once and means them
wherever the music is coming from. The remembered track describes the
VOLUME -- "carry on where I left off" means nothing applied to a card
that has never held that file.

One record on one volume made both wrong at once. A boot from the SD
card followed by playback from USB replaced the USB file with the SD
card's state, losing crossfade settings set while listening to USB; and
there was only ever one remembered track, so returning to the other
volume resumed something that was not on it. The first symptom is what
made 1103's midpoint take four sessions to exercise -- the feature under
test was being switched off by the act of choosing where to play from.

Boot precedence is SD, then USB, then whichever volume turns up first.
That was already the order `restore_last_track()` iterated; what changed
is that a second volume appearing no longer replaces what the first one
supplied.

**A bug the host test caught before the board could.** The writer task
has to read a volume before appending to it -- it needs the byte count,
and it needs the track that file is already holding -- and the first
implementation had it take the settings at the same time. Someone
changing the volume with no card in, then inserting one, would have had
that change overwritten by whatever the card remembered, **at the moment
of the save that was supposed to record it**. So the read takes two
flags rather than one, and `may_adopt` is true only from
`settings_note_path()`: a track starting is a deliberate act, and the one
moment where taking a volume's stored settings is what was asked for.

Each half is read at most once. The track the first time a volume is
seen, because reading it again would undo `settings_set_track()`, which
is newer than anything on the card. The settings from whichever volume
adopts first, and never again.

**The file format did not change.** One record, same keys, still
readable by an older build. What changed is which volumes get written and
which single field differs between them. The first boot after this patch
still loses whatever the non-adopted volume remembered, because there is
no way to merge two files that disagree without inventing a rule about
which is newer -- after that they agree for ever.

**Host-tested, not built, not flashed.** `texttest/setstest.c` models two
cards and a reboot: a crossfade set on USB survives a boot that adopts
SD, each volume resumes its own track, a volume inserted later receives
the settings without losing its track, and a change made with no card
present survives to the first one that appears. That last case is the one
that failed first time.

### The seek bar was left behind by 1103 (1109)

A regression, introduced by 1103 and found on the board: with a 12 s
crossfade the seek bar ran toward the end of the outgoing track, then
snapped backwards.

`ring_publish()` read the position from `s_ring_play`, and `s_ring_play`
stays on the OUTGOING ring for the whole overlap -- it only moves when
the crossfade completes. That was correct while the screen also changed
at the end of the overlap, because the position, the title and the
length all turned over on the same instant. **1103 moved the screen to
the midpoint and left this one line behind.**

What the glass showed, on the boundary at 275534: from the midpoint at
281554 to the end at 287563, six seconds of the outgoing track's
position -- climbing toward its 203 s length -- drawn against the
incoming track's 228 s length, under the incoming track's title. Then
`s_ring_play` moved and the bar snapped back to about 12 s.

The fix is that the position follows the same commit as everything else:
`s_visuals_released` already answers "which track is the screen showing",
so the bar reads the fill ring once it is raised and `s_ring_play` has
not caught up. Reusing that flag rather than adding a second test is the
entire argument of `track_commit()` -- four things that must change
together are one thing -- and the bar had been the fifth all along
without anyone noticing, because until 1103 it happened to change at the
right moment for the wrong reason.

**Outside a crossfade nothing changes.** An ordinary tail raises
`s_visuals_released` at the moment the tail ring empties, and by then
`s_ring_play` has already caught up, so the new test is false and the
ring chosen is the ring that was always chosen.

**What this says about 1103.** That patch grouped four things and argued
that a fifth would be a field rather than a site. It was right about the
principle and wrong about the count: the seek bar's position was a fifth
member that lived somewhere else entirely, on the writer, and grouping
the other four made its absence visible only once the timing moved. The
lesson is not that the grouping was wrong; it is that "what else reads
which track is playing" is a question worth asking exhaustively rather
than from the list already in hand.

**Host-tested, not built, not flashed.** `texttest/postest.c` checks the
pairing rather than the ring index, because the ring number is an
implementation detail and the property is that the position and the
length never come from different tracks. Every combination of play ring,
fill ring and released flag is swept, including the mirrored assignment
-- the rings alternate, so an outgoing ring 1 is exactly as common as an
outgoing ring 0, and a rule that only worked one way round would be
right half the time.

### Both ends of the boundary, and the first caller that asks about trim (1108)

Two things, and the second is smaller than it looks.

**The fade is refused at both ends now.** There was already a rule
against fading INTO a track shorter than twice the fade -- a short
incoming track never reaches full volume, and the board once ran a
10244 ms fade into a 3 s track that was inaudible. Nothing said anything
about fading OUT of one. A 24 s track under a 12 s fade is half arrival
and half departure with no middle.

The writer's `tail was shorter` clamp is not this rule. That clamps the
fade to whatever of the track is still QUEUED, which keeps the overlap
legal; it says nothing about whether a fade that long belongs on a track
that short.

**The comparison changed from `<` to `<=`, deliberately, at both ends.**
`<` let a track of exactly twice the fade through, and exactly
two-to-one is the degenerate case rather than the first acceptable one.
This alters shipped behaviour for exactly one value per fade length, and
both ends use the same comparison so they cannot disagree about a track
sitting on the boundary. The host test checks the boundary rather than a
comfortable example, which is how the ambiguity surfaced at all.

**And decoder.h finally has a caller.** It has argued since it was
written that the caller "needs to be able to ASK, and to refuse", and
until now nothing asked. `boundary_join_exact()` is the asking: a join is
sample-exact only when BOTH sides report `DECODER_TRIM_EXACT`, because
the overlap has to be positioned against the trimmed end of one and the
trimmed start of the other.

It runs at the first decoded block, not at the arming decision, and that
placement is forced: `decoder_read()` reports trim, `decoder_open()` does
not, so the incoming track's state does not exist when the fade is armed.
The outgoing track's is carried across in `s_prev_trim`, alongside
`s_prev_len_sec` for the rule above -- both are questions about a file
that has already closed.

**IT DOES NOT REFUSE CROSSFADES, and the arithmetic is why.** Encoder
delay is about 1152 samples, 26 ms at 44.1 kHz, and padding is the same
order, so the worst misalignment an untrimmed boundary can produce is
roughly 50 ms. Against a 2 s overlap that is 2.5% of the fade; against
12 s it is 0.4%. A crossfade is a deliberate blur and swallows it.
decoder.h's warning -- the incoming track entering early or late, audible
on anything with a beat -- is about a SAMPLE-EXACT join, where 50 ms of
silence is the entire defect rather than a rounding error in a blur.

**And the cost of getting that wrong is total.** Every MP3 without a
Xing header reports `NONE`, which on the board's own test library is
every file: `no Xing header, no gapless trim` on all of them. A trim
refusal wired into the crossfade would switch crossfade off for the whole
collection, in the name of an error nobody could hear. So the verdict is
logged at every boundary and gapless will refuse on it -- which is the
division decoder.h actually described. Applying a gapless precondition to
a feature that does not need one would have been the mistake.

The line exists for the collection rather than the file. A library
reporting `exact/exact` everywhere can have gapless unconditionally; one
reporting `none` everywhere needs the delay from somewhere else before
gapless is worth writing at all.

**Host-tested, not built, not flashed.** `texttest/trimtest.c` checks
both length rules against the boundary value in both directions, that
unknown lengths never refuse (unknown is not short), that a zero setting
is off regardless, and that the trim verdict accepts only exact/exact --
with `UNKNOWN` and `NONE` both failing, since the enum distinguishes them
for a reason that is not this one.

### Counting the exits, because a log line cannot investigate itself (1107)

1106's watchdog answered, and the answer was that both previous patches
were aimed at the wrong layer.

A full boot log, nothing trimmed. The first crossfade started at 167179
and 1106's check was due at 173179; the tail latched at 149743 and 1105's
was due at 209743. Both fell inside the capture. **Neither fired.** So
`s_xfade_active` and `s_tail_pending` were both false on time -- the
flags cleared normally, through one of the sites that clear them.

Every one of those sites logs. There are six, checked one at a time:

| clears | line |
| --- | --- |
| `xfade_active` | `crossfade cut short` (W) |
| `xfade_active` | `crossfade done` (I) |
| `xfade_active` | `crossfade ended on a partial frame` (W, 1105) |
| `xfade_active` | `no crossfade: %u Hz into %d Hz` (I) |
| `xfade_active` | `no crossfade: the rate changes` (I) |
| `tail_pending` | `played out` / `media gone` / `interrupted` |

A cleared flag and no line is a contradiction the source cannot produce.
**So the thing to doubt is delivery, not state.**

**What this costs, recorded rather than glossed.** 1105 fixed a hole that
was real, narrow, and not being fallen through. 1106 chased a stall that
is not happening. Two patches and four board runs on hypotheses the
instrument then excluded -- and the instrument's value turned out to be
its silence, not its output. The earlier guesses (seeks, then first-use
initialisation) both fitted every observation available at the time and
were both wrong.

**A log line cannot investigate its own delivery, but a counter can.**
Each exit increments one; the tallies are printed LATER, at the next
boundary, on a console demonstrably working because the line carrying
them arrived. `crossfade:` and `tail:` have appeared at every boundary in
every log so far, so the report rides those two.

Which makes the next reading decisive rather than suggestive:

- tallies show an exit that printed nothing -> the path ran and the line
  was lost, and the search moves to the logging path;
- tallies show no exit at all -> something clears the flag outside the
  six known sites, the grep above is wrong, and the search moves back
  into the player.

**Not atomic, deliberately.** Two tasks touch these -- the writer for the
overlap's own exits, the decode loop for the rate refusals -- but each
counter has a single writer, and they are read for comparison against log
lines rather than for control. A lock would be protecting a diagnostic
from a race that cannot change the conclusion.

**1106's limit is tighter here too.** Twice the overlap's length meant
waiting twenty-four seconds on a twelve-second fade to learn nothing. A
fixed two-second grace is tighter everywhere: 5 s for a 3 s fade, 14 s
rather than 24 s for a 12 s one. The grace still has to exceed any
stretch a refill can produce, since a starved overlap stretches on
purpose, and two seconds against a twenty-second ring is comfortable.

**Host-tested, not built, not flashed.** `texttest/exittest.c` checks the
properties that make the tally trustworthy rather than merely present: a
zero is never printed as though the exit occurred, a start without its
matching done is visible as such, all nine counters at `UINT32_MAX` stay
inside the buffer, and the order follows the enum so two reports can be
diffed by eye. A diagnostic that lies is worse than none.

### A watchdog scoped to the overlap (1106)

1105 closed a hole and the hole was not the fault. The next board run
carried the fix, printed no `crossfade ended on a partial frame`, and
reproduced the silence anyway. **The partial-frame theory is dead as an
explanation** -- it was reachable by inspection, it was never observed,
and the fix now guards a case nobody was falling through. Worth recording
plainly: two patches were spent narrowing a fault that is still open.

**What five boundaries across two sessions do say:**

| enabled | crossfade | endings |
| --- | --- | --- |
| 58254 | **87632** | **silent** |
| | 181869 | complete |
| | 382293 | complete |
| 306617 | **389219** | **silent** |
| | 751091 | complete |

The FIRST crossfade after the feature is switched on goes silent; every
one after it prints its pair. No counterexample. That kills the earlier
guess that it was the seeks -- the second session had none -- and points
at state initialised on first use rather than at ring arithmetic.

**And a correction to how the boundary was being read.** The overlap is
not between two decoding tracks. It fades the outgoing track's QUEUED
TAIL into the incoming track that is already playing, which is why
`crossfade:` lands seventeen seconds after `playing`, when the tail ring
drains to the overlap length. So the state that differs on first use
belongs to the tail path, not to track startup. Several messages of
reasoning went the other way before this was noticed.

**Why 1105's watchdog was not enough.** It measures sixty seconds from
the tail latch. In the session above it would have fired thirty-eight
seconds past the end of the capture, and it only fires at all if the TAIL
FLAG is the thing stuck -- if the overlap is stalling while
`s_tail_pending` clears normally, it never speaks.

`xfade_stall_check()` is scoped to the overlap and fires at twice its own
length: six seconds for a three-second crossfade, which is where the
missing `crossfade done` should have been. It separates the three cases
that guesswork has not been able to:

- overlap still running and unable to advance -- active set, position
  short of frames;
- overlap ended but the tail flag orphaned -- active clear, tail set;
- both cleared and only the logging lost -- neither set, and it stays
  silent, which is itself the answer.

Both watchdogs print through one `writer_state_dump()`, because they are
looking for the same fault from opposite ends and two readings that can
be compared line for line are worth more than two formats that have to be
reconciled first. The dump gained `s_tail_pending` for the same reason.

**The floor matters more than the multiplier.** A starved overlap
stretches ON PURPOSE -- the solo branch holds the ramp where it is rather
than falling through to unity -- so a watchdog that fired on a stretched
overlap would be reporting the feature working. Twice the length with a
two-second floor clears a legitimately short fit whose doubled limit
would otherwise be a handful of milliseconds.

**Host-tested, not built, not flashed.** `texttest/stalltest.c` runs the
trigger over a simulated clock: a 3 s overlap completing on time never
warns, a stuck one warns exactly once by 6 s and never again, the state
re-arms for the next overlap, a 20 ms fit is silent inside the floor, and
a 12 s overlap is silent at 20 s and warns at 24. The false-positive
cases are the point; the true positive was never in doubt.

**Not a code change: the crossfade settings were saving correctly all
along.** `record_line()` writes `crossfade` and `crossfade_album` through
one shared `SETTINGS_FIELDS_FMT` used by all three exits, so both keys
reach every record of the main dotfile. What loses them is the
volume-follow overwrite -- `/sd` adopted at boot, `/usb`'s richer file
never read, the next save replacing it -- which is a policy question and
is left open rather than decided here.

### A crossfade with no exit (1105)

Three lines are supposed to be exhaustive at a crossfade boundary: the
overlap completes (`crossfade done`), or it is cut short, or the ring it
was fading out of runs dry (`the finished track has played out`). A
boundary in the second board log printed none of them:

    69194  tail: 3518 KB of this track still to play; holding the screen
    87632  crossfade: 2000 ms, trim out 100% in 100%
    88659  no text tags in this file; showing the filename
           ... nothing ...

The other three boundaries in the same log all printed their pair.

**The screen changed anyway, and that is 1103's doing.** The visuals
released at the midpoint, 1027 ms into a 2000 ms overlap, so nothing
downstream noticed the writer state was where it should not be. Under the
old end-of-overlap timing this would have shown as the screen never
changing. 1103 turned a visible failure into a silent one, which is right
for the viewer and is why this needed looking for rather than seeing.

**The hole.** In the overlap block, `n` is the frame-aligned minimum of
the two rings and `solo` is the frame-aligned outgoing ring. If the
outgoing ring holds one, two or three bytes, all three exits miss it: not
empty, so the handoff at the top of the loop does not fire; not a frame,
so neither the mix nor the solo path can take it. The code fell out of
the crossfade block and did the ordinary single-ring receive **with
`s_xfade_active` still true** -- writing at unity partway down a fade,
which is the exact artefact the solo branch above it exists to prevent
and whose comment says so.

**How a ring comes to hold a partial frame**, which is what makes this
reachable rather than merely arithmetic. Every write is whole frames, so
the remainder cannot come from a write that succeeded -- only from one
that did not finish. `xStreamBufferSend()` returns a short count when the
ring fills, and the decode loop's send loop breaks out on `s_seek_pct` or
`s_pending_ready` with `remain` outstanding. A seek is followed by
`s_pcm_flush`, which drains both rings and restores the alignment. **A
pending track change is not.** So one to three bytes of a frame can cross
a track boundary and land in the next overlap.

**What is NOT established.** That this is what happened at 69194. The
mechanism is reachable by inspection; it was not observed, and the log
cannot distinguish it from the other ways those three lines could have
gone quiet. The old code was also not permanently stuck -- the ordinary
receive would drain the stray bytes and the handoff would fire on the
pass after -- so the visible cost is one chunk of unity-gain audio inside
a fade, not a hang. That is a smaller claim than "found it".

So two things here. The exit is closed: the overlap ends explicitly, at
the fade's current gain, with a line that names the byte count. And
`tail_stall_check()` fires once if `s_tail_pending` outlives sixty
seconds -- unreachable by any amount of audio a twenty-second ring can
hold -- printing every ring's occupancy, which ring the tail is in, and
where the overlap had got to. If the fall-through was the cause it will
never fire; if it was something else it names it, instead of leaving the
next reading to another 114 seconds of inference.

**Host-tested, not built, not flashed.** `texttest/xfadetest.c`
transcribes the exit arithmetic and sweeps both ring occupancies 0..64:
the ordinary cases still reach mix, solo and handoff where they did, a
sub-frame remainder reaches none of them, and **no other combination
falls through**. That last one is the useful half -- it says the hole is
exactly one case wide and the fix does not need to be broader.

### The screen changes at the crossfade's midpoint (1103)

Four things describe the playing track -- the title row, the cover, the
envelope under the seek bar, and the chooser's playing marker -- and they
are drawn by three different mechanisms. For most of this file's history
they were four statements that happened to sit next to each other, and
twice they came apart: the bar was published at `track_change_begin()`
while the title waited for the handoff, and the chooser worked its marker
out from `playlist_current()`, which is where the decoder is rather than
where the speaker is. Both read as a bug in one of the four rather than
in the arrangement.

**They are one function and one struct now.** `track_commit()` takes a
`track_commit_t` and publishes the lot in one pass on the decode loop.
Adding a fifth thing means adding a field rather than remembering a site.
`VISUALS_GATE()` is still a macro because it has to close over the decode
loop's locals in two places, but it now does nothing except build the
struct and ask `track_commit_due()`.

**And the timing was wrong whenever a crossfade was on.** The gate fired
on `!s_tail_pending`, which clears when the outgoing ring runs dry.
Without a crossfade that is exactly right. With one it is late by the
whole overlap, because the outgoing ring is drained *by the mix* and does
not empty until the fade is over -- so a five-second crossfade played
five seconds of the new song under the old song's title, cover, envelope
and highlight, and then changed all four at the moment there was nothing
left to change for.

**The midpoint is where it belongs, and not as a split-the-difference.**
`xfade_mix()` is equal-power: at the halfway frame the two tracks are at
equal gain, and that is the frame at which what you are hearing stops
being one song and starts being the other. It is also the only point in
the overlap defined without deciding first which track is "playing".

So `s_visuals_released` -- one volatile bit, raised by the writer,
consumed by the decode loop, the same shape as `s_tail_pending` and for
the same reason. Lowered in exactly one place, `track_change_begin()`,
which matters because a crossfade raises it while the *previous* track's
decode loop is still running and it must not survive into the next one.

- **Tested on the advanced position, not the previous one**, so an
  overlap shorter than one chunk still releases instead of silently
  falling back to the end-of-overlap timing this replaces.
- **The tail-dry site still raises it**, which is both the no-crossfade
  path -- unchanged, to the instant -- and the backstop for an overlap
  cut short by an outgoing ring that emptied early. Without that, a cut
  short crossfade would wait for a halfway point that is no longer
  coming and hold the old track's name for the whole of the new one.

**Host-tested, not built, not flashed.** `texttest/committest.c` runs the
extracted rule through eleven cases: play-from-stopped, tail without
crossfade, the midpoint, a sub-chunk overlap, the cut-short backstop,
survival across a track change, and monotonicity within one overlap. Two
cases are marked EXACT and exist only to catch this changing the timing
of an ordinary track change, which it must not.

**What to watch on the board.** The prediction is that with crossfade set
to 5 s the title and cover change about 2.5 s into the overlap rather
than at the end of it. If they still change at the end, the midpoint is
not being reached -- look at whether `xfade_mix()` is running at all
before assuming the flag is wrong.

### One album, one picture (1102)

1006 measured three cache slots holding 10935 KB and stopped there,
because every fix it could see was a decision about what the player
should do. It listed three and took none.

**There was a fourth, and it is the one that changes nothing.** An album
has one cover and its tracks are consecutive, so previous, current and
next are three paths pointing at the same picture -- and the cache was
storing it three times because nothing ever asked whether it already had
it. Covers are refcounted blobs now: the store path hashes the incoming
bytes, finds a held blob with the same hash and length, confirms with a
memcmp, frees the duplicate and takes a reference.

Every caller still gets back exactly the bytes the file contained. No
cover is capped, downscaled, or not prefetched. That is the whole
argument for doing this one before the other three: it is not a policy
choice, so it does not pre-empt the policy choice, and the policy choice
gets made against a smaller number.

**What it does not do.** Three tracks with three genuinely different 4 MB
covers still cost 12 MB. The worst case is untouched. This helps the
common case and is honest about being nothing else -- and if an album's
files were retagged one at a time, the pictures may not be byte-identical
and it will do nothing at all. `prefetch done` now prints `+N KB shared`
so that case is visible rather than assumed; **`+0 KB` across a whole
album is the falsification condition**, and it means the sharing premise
is wrong on real files even though it is right in principle.

Four things that are load-bearing:

- **The hash runs outside the lock, and that is not tidiness.** It is
  megabytes of Murmur2 -- tens of milliseconds on the cover 1006
  measured -- and the decode loop takes this same mutex through
  `mediacache_tags()` at the instant of a track change. Hashing inside
  it would hand the one latency-critical caller a stall proportional to
  the size of someone else's album art. The bytes are the caller's own
  and no other task can see them, so there is nothing to protect.
- **The memcmp is not optional, and the reason is a change of stakes.**
  1011 wrote that equal-length covers hashing the same are the same
  image "for every purpose this program has", and for a log line that is
  true. This is not that purpose: a collision here puts one album's
  picture on another album's screen and nothing downstream would notice.
  32 bits is a fine filter and a poor proof, so it is used as a filter.
  It runs only on a hit, which is the path about to save a whole copy.
- **`release()` had to become an unref.** Freeing outright would leave
  the other two entries on an album pointing into freed memory the
  moment one was evicted -- and they would go on answering
  `mediacache_art()` with it, because nothing else says the pointer is
  dead. This is the failure the test suite is mostly about.
- **`cover_hash()` is `albumart_cover_hash()` now**, because two callers
  that must agree should not be two functions. Which also hands 1011b
  the thing it was missing: `mediacache_art_hash()` answers for the
  prefetched cover, so a track change finally has something to compare.
  Nothing compares them yet -- 1011b still needs somewhere to re-blit
  from, which is unchanged.

**Host-tested, not built and not flashed.** The real `mediacache.c`
against the real `mediacache.h` and `albumart.h`, stubs only for the IDF
headers, under ASan and UBSan: fourteen cases covering sharing, eviction
of one sharer while another is pinned, replacement under one path,
idempotent re-store, the all-slots-pinned refusal, and teardown. Leak
detection clean, which is the check that matters -- the refcount's
failure modes are a double free and a leak and both are silent on the
board until much later.

The gap that remains is the one this file keeps recording: a host harness
compiles function bodies, not the file they sit in. The definition
ordering was checked by hand for the same reason 1011's nearly was not.

### The cover hash is Murmur2, and 32-bit on purpose (1011)

The cover's identity hash was MD5 from ROM, **ungated**: 1.8 MB hashed
on every cover decode, for a log line. The comment above it already
conceded the point -- "collision resistance is irrelevant when the
question is are these the same bytes twice" -- and then paid for the
collision resistance anyway.

`cover_hash()` is MurmurHash2 now. The question is identity, which wants
distribution and not cryptographic strength, and the diagnostic it
exists for is unchanged: hashing `in` rather than the source still
covers the ID3 extraction, the read off the card and the memcpy, which
is what distinguishes bad bytes from a bad engine.

**The 32-bit variant, and that is the interesting choice.**
MurmurHash64A is the better hash and it is built on 64-bit multiplies,
which on this RV32 target are calls into `__muldi3` -- the same class of
fault 1005 hit with `__moddi3` and 1007 wrote down, in the same kind of
loop. **A hash over a megabyte is precisely the shape that turns a
software 64-bit op into real time**, so the rule from 1007 got applied
before the mistake rather than after it, which is the first time in this
series that has happened.

Details that are load-bearing rather than tidy:

- **The 4-byte load is a `memcpy()`, not a cast.** The buffer is DMA
  reachable and nothing here promises its alignment; an unaligned 32-bit
  load is a fault on some targets and a silent slow path on others. The
  compiler folds it back into one load where it is allowed to.
- **Length is mixed into the seed**, so a truncated read does not hash
  the same as the whole image. Verified, not assumed.
- **Public domain** (Austin Appleby), so it adds no licence obligation --
  unlike the TJpgDec fallback, which does.

Verified on the host under ASan and UBSan against the real cover size:
deterministic, agreeing between aligned and unaligned copies, 2000
single-bit flips all detected, truncation detected, and every tail
length 0..7 exercised. The function extracted from the file reproduces
the reference hash exactly, which is the check that the thing in the
tree is the thing that was tested.

**It was also nearly a build failure.** Written in place, `cover_hash()`
landed 270 lines below its caller -- the use-before-definition this file
records twice, once as a shipped break and once as "this file has
shipped a build failure for exactly that before". Caught by looking
rather than by compiling, since the host stubs do not reach
`albumart.c`.

**And the hash only exists on the decode path.** `cover_hash()` runs
inside `albumart_draw()`, which is reached for the cover being
*displayed*; `prefetch_next()` stores the compressed image without
hashing it, and the board shows exactly that -- one `jpeg in: ... hash
04d36f37` for the playing track and none for the prefetched one, both
1871582 bytes. So a comparison at a track change has nothing to compare
against yet: whatever 1011b turns out to be, it has to hash where the
image is fetched as well as where it is drawn, and hashing 1.8 MB on the
prefetch path is a cost that belongs in that decision rather than
assumed into it.

**What this does not do yet.** The hash is not compared against
anything. Skipping the decode when a track's cover matches the one
already on screen needs somewhere to re-blit from: `ui_clear_art()` runs
on the decode loop *before* `do_art()` is handed the image, so a skipped
decode leaves a blank square unless a decoded copy is retained. That
contradicts `mediacache.h`, which declines to cache the decoded cover
because the codec rebuilds it in milliseconds -- an argument whose
premise has since changed, since the 3000x3000 case takes hundreds of
milliseconds when it works and fails outright when it does not. That is
a decision, not an oversight, and it is not taken here.

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

### The USB rail turns itself off if nothing turned up (1100)

VBUS is driven unconditionally at boot and stays that way, because a UAC
headset cannot announce itself through a dark port -- which is why the
old "only power USB if the card is unreadable" rule had to go. The cost
is a rail driven all day for a port with nothing in it.

**After one track has actually played, the port is asked once whether
anything arrived, and if nothing did the rail drops.** A track is long
enough that anything has had every chance to enumerate -- the slowest
mount observed was 2.7 s after boot, against a track measured in minutes
-- and it is a moment the listener is not waiting on.

**Once, and never automatically back on.** `s_usb_autooff_done` is set
whether or not the rail moves, and that is the part worth keeping: it is
what stops this fighting the settings panel. If the listener turns the
port on afterwards, that is a request with a person behind it, and
nothing should second-guess it at the end of the next track. **Automatic
off is a power saving; automatic on would be an argument.**

**Three attachment questions, not one.** A mounted volume is the obvious
one and the other two are the ones that would have made this a bug:

| Predicate | Catches |
| --- | --- |
| `storage_present(STORAGE_USB)` | a mounted drive |
| `uac_present()` | a USB audio device |
| `hid_present()` | **a remote, which announces itself to nothing else** |

`hid_present()` is new, added here for exactly this. `hid.c` already
tracked open devices in `s_open[]` -- it had to, since 0902 replaced a
single handle with a table -- so the predicate is that table read under
the lock it already has. Without it a plugged-in remote is invisible to
every "is anything attached" test in the program and gets its power cut
mid-press, which is the kind of fault that would have been reported as
"the buttons stop working after the first song" and taken a while to
connect to a power patch.

`storage_usb_power(false)` and not `usbhost_set_power(false)`: cutting
VBUS under a mounted volume is a physical unplug as far as FatFs is
concerned, and the former unmounts first and refuses outright while the
volume is held. The busy check beside it is belt-and-braces, not the
safety.

`blocks > 0`, so an unreadable file does not count as the track. Three
of those in a row is a stopped player, and dropping the rail underneath
that would take the drive away from someone trying to work out why
nothing plays.

**Not flashed.** The host stubs reach neither `player.c` nor `hid.c`. The
gate was extracted and run against all eight attachment combinations plus
the two sequencing cases -- second track, and manual re-power after an
auto-off -- confirming among other things that `storage_usb_power()` is
never reached until every predicate has cleared, since it is the only
term in that chain with a side effect and C's short-circuit order is what
guarantees it runs last.

What to look for: `USB bus power off: nothing attached after a track` at
the end of the first track with an empty port, the port label in settings
reading `off`, and **no such line** when a stick, a headset or a remote is
plugged in.

## Building this from Android, under Termux

The build host is a phone. That is not a footnote -- it explains why so
much of this file says "not flashed" and why the host-stub test suite
matters more here than it would elsewhere.

**It has to be two environments, and the reason is not preference.**
ESP-IDF's prebuilt toolchains are glibc-linked `linux-arm64` binaries
and Termux is bionic, so they will not run natively and there is no
`riscv32-esp-elf` in Termux's own repos. proot gives you glibc. But
proot cannot reach USB, and Termux gives you no `/dev/ttyUSB*` node
without root. **So: build inside proot, flash from Termux proper.**

Build, in `proot-distro login debian`:

    apt install -y git wget flex bison gperf python3 python3-pip         python3-venv cmake ninja-build ccache libffi-dev libssl-dev         dfu-util libusb-1.0-0
    git clone -b v5.5.5 --recursive https://github.com/espressif/esp-idf.git
    cd esp-idf && ./install.sh esp32p4 && . ./export.sh

Budget 8-10 GB. Keep the project somewhere Termux can also see, so
`build/*.bin` is reachable from outside proot.

Flash, from Termux, through a USB-serial-to-TCP bridge app and a pty:

    socat pty,link=$HOME/esp32,raw,echo=0 tcp:127.0.0.1:8080 &
    esptool --chip esp32p4 --port $HOME/esp32 --baud 460800         write_flash @flash_args

`idf.py monitor` takes the same pty, which is where every log quoted in
this file comes from.

`nrflash` is the Termux-native alternative and avoids the bridge app,
but **it does not support the P4 today** -- the stub binary is in the
package, the chip parameters and magic value are not.

### What this costs, and what follows from it

- **The cable is a real failure mode.** Charge-only cables and cables
  that brown the board out both present as software faults. Rule it out
  first.
- **Phone sleep kills long builds.** `termux-wake-lock`.
- **A build is expensive enough that it is not the fast feedback loop.**
  That is why `texttest` and `ctrltest` exist and why patches here get
  extracted and run in isolation on a host rather than compiled in
  place. It is also why "not flashed" is stated on every patch rather
  than assumed: the gap between written and run is wider here than the
  usual one, and 1005, 1009 and the 0200 series were all found by a
  board doing something a reading had not predicted.
- **Compiling is worth more than it looks.** 1101's `esp_jpeg` call has
  never been through a compiler, and a build alone -- no flash, no
  cable, no bridge -- would settle the enum spellings it is most likely
  to have got wrong.

## Where v0.4.0 is going (the network series)

v0.4.0 is internet radio, and everything below it is scaffolding for
that. Station data from https://www.radio-browser.info -- an open API
with no key, which is why it was picked: no credential to store, no
account to manage.

### Landed and running on hardware

- **The radio comes up.** The P4 has none of its own; the C6 beside it
  speaks ESP-Hosted over SDIO2. `wifi_start()` powers the module through
  P0 of the expander at 0x44, sets the SDIO pins from the schematic,
  connects to the slave and starts esp_wifi as a station. About two
  seconds, most of it the C6's reset settle. `wifi_stop()` unwinds it in
  order and cuts the rail, ~140 ms.
- **The NET tab's switch acts when pressed**, via `wifi_request_apply()`
  posting to a worker -- `panel_touch()` runs on ui_task and must not
  block for two seconds over live audio.
- **`wifistore`** holds up to eight networks in NVS, with 10072 host
  checks under ASan. Its writer is the portal's success path, below.
- **The clock floor.** `.defeatist.dat` carries `ntp_epoch` and
  `ntp_boot_us`; the stored belief only ever moves forward, seeded from
  the build timestamp so a player that has never seen a network still
  refuses 1970. Written on every save, so uptime accumulates across
  reboots with no network at all.

### Built, and flashed once (0003-0009, then 0010)

First hardware run, 0003-0009 on v0.3.0-53: **the portal works end to
end on this board.** APSTA came up on M5's 0.0.0 slave, a phone got a
lease and 86 DNS answers, the form was submitted, the join succeeded,
the network was saved, NTP synced 1.4 s after the address, and the AP
came down after the SAVED linger. The log is quoted in `portal.h`.

What that run measured, beyond "it works":

- **Reason 2 (AUTH_EXPIRE) on a first attempt, whatever the secret.**
  In the portal the derived PSK failed with reason 2 and the passphrase
  that followed joined, which looked like "transition APs refuse a PSK".
  The second flash took that back: at boot the *saved passphrase* failed
  the same way after 6.6 s and joined on the retry a minute later.
  0011 retried with a bare `esp_wifi_connect()` at once, and 0012 after
  a 5 s pause; both got reason 205 with rssi -128, a connect that never
  reached the AP. Every success had a fresh `esp_wifi_set_config()`
  first -- the worker's minute-later retry, and the portal's passphrase
  13 ms after the PSK failed. 0013 makes the retry a whole attempt, no
  pause, and **the fifth flash confirmed it**: reason 2 at 13557, retry
  from set_config at 13570, address at 17564 -- 13.2 s after the radio
  came up, where the same boot took 76 s before. NTP synced 4.5 s after
  the address. Four reason-2 failures followed event id 43, but the
  sixth flash had one without it, so that is not the cause.
- **Sixth flash (0014+0015):** boot join again 13.2 s from radio up,
  NTP 0.9 s after the address. Setup started while already connected to
  fivescore ran three attempts: PSK reason 2, PSK again reason 202,
  passphrase joined -- 11 s from submit to address. The PSK reaching
  authentication has now been refused with 202 twice. **0017** skips
  the PSK on networks the scan shows as WPA3 or WPA2/WPA3 and tries the
  passphrase alone; WPA2-only and unscanned networks keep PSK first,
  because passphrase-first there would store a 64-hex key no join on
  this C6 has used yet. Unflashed.
- **A phone hotspot a tablet lists never appeared in a scan.** 0018
  scans actively at up to 300 ms a channel (IDF's default is 120), keeps
  hidden networks and logs them with channel and BSSID, ends each scan
  with a summary line, and replaces the form's `<datalist>` with a
  `<select>` of the scan (name, security, dBm, channel) plus a text
  field for a hidden or unheard network; a typed name wins. Unflashed:
  whether the hotspot then appears is the question it was built to
  answer. **Answered, and not by the scan:** the phone's hotspot is set
  to "prefer 2.4 GHz", but a Wi-Fi analyser shows it only ever on 5 GHz
  -- the phone falls back when 2.4 GHz is crowded, and 32+ networks per
  scan says it is. The C6 is a 2.4 GHz radio and will never hear it.
  The longer dwell, the hidden entries and 0020's larger cap all stay:
  they are what made "not heard" distinguishable from "not kept". A
  phone's "preferred" band is not a setting to rely on.
- **Eighth flash: the first join with a derived PSK.** `Suscore` is a
  WPA2-only guest network on the same router as `fivescore` (both on
  channel 10 that day). The portal took the WPA2 path, joined with the
  derived PSK on the first attempt, and saved it as a PSK -- the first
  64-hex key this C6 has joined with. One router, not yet a general
  result. Two saved networks now, both in range; the next boot tests
  the stored PSK and the ranking. Seventh flash: both scans came back exactly 32 networks with
  16-17 hidden -- the old cap, full, cutting the weakest -- and still no
  hotspot. 0020 raises the cap to 64 and warns when a scan is cut.
  With 0020 flashed one scan still came back exactly 32 with no
  warning, and 0033 wrote that down as a limit below this code. **The
  next flash took that back:** 42 networks (23 hidden) and 38 (18
  hidden), the weakest at -94 dBm. There is no 32 cap underneath; that
  room just had 32 that time. The
  same flash had the first boot-time-style join that worked on the
  first attempt (after Wi-Fi off/on, no reason 2, id=43 present), so a
  first attempt does not always expire. 0014's pause was
  not exercised on that flash; on v0.3.0-76 it was: `pausing playback
  for network setup`, then the amplifier went idle. The refused play
  press is still unseen.
- **Setup started during the boot join (v0.3.0-76, fixed in 0033).**
  START four seconds into the worker's join of fivescore gave `portal
  up ... (0 networks listed)`: the driver refuses a scan while the
  station is connecting, and the failure was silent. The worker's join
  also carried on under the running portal and retried into it.
  `wifi_scan_list()` now takes the join lock, so the portal's scan waits
  for the attempt to end, and a refused scan is logged. Reading that
  code turned up a worse one: `wifi_sta_ssid()`, called by the NET tab
  on ui_task, took the same lock, which a join holds for up to thirty
  seconds -- a join begun while connected could freeze the screen. The
  SSID has its own short lock now.
- **STOP during setup's scan (v0.3.0-77, fixed in 0034).** The scan is
  about four seconds; STOP pressed in it was only acted on after the AP,
  DHCP and the web server had all come up, half a second before they
  were torn down. `bring_up()` now checks for a stop after the scan and
  raises nothing. 0033's scan-waits-for-join could not be seen on that
  flash: setup was started with the station already joined.
- **Closing the panel while paused left it on screen (v0.3.0-78, fixed
  in 0035).** CLOSE brought the transport bar back but the NET tab
  stayed painted above it, with setup running or not -- because setup
  had paused playback. Paused mid-track, the ring fills and the decode
  loop sits in its send loop a slice at a time; the repaint it services
  is at the top of the loop, which it never reaches until play resumes.
  The send loop now services `s_repaint_art` in place (not by breaking
  out, which would drop the half-sent block). The chooser's cancel
  while paused mid-track had the same path. Also in 0035: the panel's
  CLOSE is greyed and refused while setup runs, since setup holds
  playback paused and a hidden setup is a player that will not play
  with nothing saying why. STOP first. **Confirmed on v0.3.0-79:**
  paused mid-track, the chooser's cancel, the panel's CLOSE and the
  Sleep page's CLOSE each redrew the art at once.
- **The PSK is refused by a WPA2/WPA3 transition network.** Fourth
  flash: reason 202, AUTH_FAIL, then the passphrase joined and was
  stored. PSK-first keeps the passphrase off the device only on
  WPA2-only networks. Withholding SAE for the PSK attempt is untried.
- **Playback and setup (0014).** Setup no longer refuses to start
  while a track plays: it pauses the track, play is refused while setup
  runs, and nothing resumes afterwards. Next, prev and the chooser still
  change the track, which loads paused. Unflashed.
- **The "a track is playing" refusal fired with nothing playing.** Right
  after boot the player sits on "ready to resume" with `s_playing` at its
  initial true, and `s_ring_pct` is 0 from an empty ring, so
  `s_playing && s_ring_pct >= 0` was true. 0010 uses `s_decoding`, which
  is true only while a decode loop exists.
- **Radio off and on again from the NET tab** tears ESP-Hosted down and
  brings it back cleanly, same MAC, twice in one session.
- **`esp_wifi_set_storage(RAM)`** returned OK -- no warning printed. That
  shows the call was accepted, not that the C6 honours it.
- **The E-level `major version mismatch -- OTA coprocessor from host`**
  prints at every radio start and changes nothing. Do not OTA the C6;
  see the manifest.

Second flash (0010): **the saved network joins at boot without the
portal**, but only on the one-minute retry, because the first attempt
hit reason 2; NTP synced 1.9 s after the address. That run therefore
also showed the retry working.

Not yet seen on hardware: more than one saved network, a refused password
on the phone's page, the non-ASCII hint, and whether the phone's
sign-in sheet opened by itself or the page was opened by hand.

- **Joining.** `wifi_join()` joins one network and reports an address,
  a refusal (`ESP_ERR_WIFI_PASSWORD`), an absence (`ESP_ERR_NOT_FOUND`)
  or a timeout, and never retries on its own. The worker joins the
  strongest saved network (`wifistore_best()`) when the radio comes up
  and every minute after while it has no address. The track loop's
  settings push never joins -- it only wakes the worker, because a
  fifteen-second join on the decode task is a stall.
- **NTP** is called from `IP_EVENT_STA_GOT_IP`. `sntp_start()` lost its
  `unused` attribute.
- **APSTA.** `wifi_ap_begin()` / `wifi_ap_end()` switch a running radio
  between APSTA and STA. **Answered on hardware: M5's 0.0.0 C6 firmware
  accepts it** (`APSTA up`, twice in one session).
- **`portal.c`**, to `portal.h`'s settled decisions. Scan as a station,
  raise `Defeatist-XXXX`, offer the AP as DNS in the DHCP lease, answer
  every name with the AP (`dnsreply.c`), serve a form listing the scan
  (`<datalist>`, so a hidden network can still be typed), and join
  before storing: derived PSK first, passphrase on refusal. Five-minute
  timeout, playback paused while it runs, state copied out.
  Everything that parses a byte from a phone -- form decoding, SSID
  escaping, the credential lengths, the DNS parser -- is in
  `portalweb.c` and `dnsreply.c`, host-tested in
  `texttest/portalwebtest.c` (57 checks plus 200000 random packets).
- **The NET tab** has an "Add a network" row: START/STOP, the AP name,
  what the portal is doing, phones joined and time left; when idle, the
  joined network or how many are saved.

**0004** tightens three things 0003 left loose:

- **The driver no longer writes credentials down.** `wifi_start()`
  sets `esp_wifi_set_storage(WIFI_STORAGE_RAM)`. Without it every
  `esp_wifi_set_config()` -- a mistyped attempt, the raw WPA3 passphrase
  -- was saved in plain text in the driver's NVS, probably on the C6,
  outside wifistore. Found by reading the map project, which does the
  same with `WiFi.persistent(false)` on this board.
- **Every saved network in range is tried**, strongest first, until one
  joins (`wifistore_rank()`, copies not indices, 16 new host checks).
  Up to eight are stored as before. A saved network whose password
  changed no longer shadows the others.
- **Autocorrect characters get their own answer.** A curly apostrophe,
  en dash or no-break space is `PORTALWEB_NON_ASCII`, and the form names
  it from a table ("a curly apostrophe (U+2019) -- the router probably
  wants '") instead of claiming a length problem. Every submission logs
  an encoding diagnosis: byte and character counts, whitespace at the
  ends, a capital first letter, and the code points of anything
  non-ASCII. Unlike the map project's line it gives no first or last
  character and no key fingerprint.

Known gaps, in the order a flash would hit them:

- **The phone may lose the setup AP during the join.** One radio, one
  channel: joining moves the AP to the home network's channel. The
  player's screen shows the result either way.
- **Open networks cannot be saved.** wifistore requires a secret. The
  form says so rather than failing later.
- **Turning Wi-Fi off from the track loop while the portal runs** waits
  for the portal to come down, which blocks that caller. Setup holds
  playback paused, so this needs a track changed with next or prev
  during setup and the switch then thrown.
- **No captive-portal DHCP option (114).** Phones find the page through
  the DNS lie and the redirect; option 114 would be quicker on newer
  Android and is a follow-up once the basic path is seen working.

### Open, in the order they unblock each other

- **Whether the clock is load-bearing.** If radio-browser and the
  streams are HTTPS, an unset clock fails certificate validation and
  nothing plays -- which is the argument NTP was added on. If they are
  plain HTTP, NTP drops to "nice for file timestamps". Worth checking
  before treating it as a dependency.
- **The sleep timer.** It has a home now (0024): the moon on the
  transport bar opens a Sleep page (`sleeppage.c`), and its first row is
  a settings-style switch, "Screen [ON]". Switching it off fades the
  backlight out over 800 ms (0025), squared, so the OFF is seen before
  the page closes; waking is a touch, instant. Under it (0026), a
  Brightness slider, 5-100%, saved as `brightness` in .defeatist.dat
  and mapped to PWM duty through gamma 2.2, because the fixed 80% duty
  looked like a plateau. The default, 90, is 79% duty, so a card without
  the key looks as before. The duty curve is an estimate: releasing the
  slider logs `brightness N% -> duty M%`, which is how to find where
  this panel actually stops getting brighter. **Measured: the bottom is
  the cliff** -- 5% maps to 1% duty and the backlight goes no lower,
  which is still bright beside a bed. 0027 carries the slider below
  that with a pixel filter (`brightness.h`): under the floor the
  backlight holds at 1% and gfx.c scales every pixel by filter/256 as
  it blits, into a scratch band, leaving the shadow buffer alone. The
  minimum was 3 (1% duty, filter 11/256) and is 6 since 0029, as seen on
  the device (filter 53/256); RGB565 loses red and blue below about
  8/256. Crossover is at 13. **0029 also made the screen-off fade
  smooth**: 0025's stepped whole percent, so it stair-stepped at the
  bottom and did nothing at all from a setting on the 1% floor (1% to 0
  is the cliff). It now fades light as (1-t)^2 -- backlight counts down
  to the floor, then the pixel filter to black, against the clock --
  and logs frames and reblits. A reblit is far slower than a backlight
  write, so the filter end of a fade has few frames; the log says how
  few.
- **Next for brightness: interpolate duty and filter, don't switch.**
  Good enough for now, but both places hand off at a hard edge.
  *The fade:* a filter change is a full reblit and a backlight write is
  nearly free, so step the filter coarsely -- 12 to 16 steps spread over
  the whole fade rather than crowded into its last tenth -- and on every
  16 ms frame set duty = wanted light / current filter, so the backlight
  fills in between reblits and the product tracks the curve. Only the
  very bottom, with the backlight pinned at its floor, can still step.
  *The slider:* level 13 is 1% duty and filter 256, level 12 is 1% and
  241, so the picture changes character at one notch. Blend across a
  band of roughly 1-4% duty, both moving. Lean on the backlight for as
  long as it has range, because RGB565 has five bits of red and blue and
  dark colours band and shift under a heavy filter; the fade does not
  care, it ends black. Both belong in `brightness.h` as pure functions,
  and `brightnesstest` can require that duty times filter follows the
  curve to within one backlight count at every frame for a given filter
  schedule. Needs a flash to judge. The timer's options go below
  it.
  **The relative timer is written (0031).** A third control on the
  Sleep page: a slider of 15-minute steps, off to 2 h, started on
  release so dragging past 2 h on the way to 30 min never runs a
  two-hour timer. It shows time left while it runs. The last 20 s ramp
  the output volume to 0 -- ending at the deadline, not starting there
  -- then the track pauses and the screen fades off. The output is left
  at 0 and restored by the next play press, so the writer's last few
  milliseconds cannot leak out at full level. Any press during the ramp
  cancels the timer and puts the volume back. Deadline on esp_timer, not
  saved across reboots. Arithmetic in `sleeptimer.h`, tested by
  `sleeptimertest`. **Seen working on hardware:** ramp started at
  1276620, `ran out: paused` at 1296187 (19.6 s -- the ramp begins on the
  first ui_task pass inside the window), screen faded, amplifier idle
  1.6 s later, woke on a touch 228 s after. The screen fade logged `45
  frames, 1 reblits, 830 ms`: from a bright setting the filter phase got
  a single reblit, which is the stepped end the duty/filter
  interpolation item below is for. At volume 21 the ramp has only 21
  levels to cross in 20 s; whether that is audible as steps is not yet
  reported. **Second run (v0.3.0-79):** set to 30 min, replaced with 15
  at 258718, ramp from 1138900, `ran out: paused` at 1158781 -- 63 ms
  after the 15 minutes, across three track changes and three
  fade-in-over-a-recorded-fade crossfades. Screen fade from brightness
  53: 43 frames, 2 reblits. Woken and played 243 s later. The volume
  restore had no log line, so whether sound came back could not be read
  from the log; 0036 adds `sleep timer: volume N restored for play`. "Until 07:00" still needs the wall clock;
  resolve the deadline to a monotonic reading once, at set time, or an
  NTP step silently changes its length. (This entry used to point at a DST write-up in
  settings.h. There is none.)
- **The stream probe (0037), before any of the stream path.** A
  throwaway `streamprobe.c`: once per boot, 3 s after the first address,
  it opens WNZK on Zeno.FM (`https://stream.zeno.fm/erunhwj5lekvv`),
  follows redirects by hand so every hop logs, sends `Icy-MetaData: 1`,
  and reads for 30 s. It logs each hop's status, connect+TLS and header
  times, the ICY and content headers, internal and PSRAM heap before,
  connected and after, whether mbedTLS checks certificate dates and
  whether the clock was set, what the first audio bytes are, the first
  three stream titles, and KB/s every 5 s. No decoding, no UI, and
  playback carries on beside it. `streamsniff.h` (pure, tested by
  `streamsnifftest`) does the byte sniffing and ICY title parsing.
  Remove it when the stream path lands.
  **First run (v0.3.0-?, joined fivescore):**
  - *Redirect:* `stream.zeno.fm` answers 302 to
    `stream-285.surfernetwork.com/erunhwj5lekvv?zt=<JWT>`. The token in
    the query string very likely expires, so a reconnect must go back to
    the Zeno URL, never reuse the redirected one.
  - *Timing:* hop 1 connect+TLS 706 ms, headers 102 ms; hop 2 connect+TLS
    1531 ms, headers 416 ms. First audio byte about 2.8 s after starting.
  - *Stream:* `content-type: audio/aac`, first bytes ADTS, `transfer-
    encoding: chunked`, `icy-name: WNZK-AM`, `icy-metaint: 16000`, title
    `" - "` (empty). ADTS AAC is within esp_audio_codec.
  - *Clock:* NTP had synced 0.6 s before the probe, so a TLS connect with
    no clock was not tested -- but mbedTLS here does not check
    certificate dates, and both certificates validated from the bundle.
  - *Heap:* internal free 101 KB before, 56 KB with one TLS session up
    (about 45 KB a session), minimum seen 41 KB, largest internal block
    32 KB throughout. PSRAM untouched: mbedTLS allocates internal. After
    closing, 93.5 KB -- the 8 KB gap is most likely the probe task's own
    stack, not yet freed when the line printed.
  - *Throughput:* a steady 50-58 KB/s for 30 s, 433 kbit/s. Far more than
    an AM talk station's AAC should need, and it did not settle, so the
    server is sending faster than real time, or the stream really is
    that rate. Counting ADTS frames would tell which; not done yet.
  - *ESP-Hosted:* `eh_sdio: mempool OOM start (RX)` twice at 50 KB/s,
    each recovering within a millisecond. Nothing was lost that the log
    shows, but it is the host's RX pool running dry at a modest rate.
  **0039 changes the probe** to answer what that left open: it starts 45 s
  after the address (time to start a track from the card, so the
  download is measured beside playback) and reads for 60 s; it logs the
  redirect's Location whole and decodes its token (`zt=` is a JWT) to
  print `exp` and `iat` against the clock; and it counts ADTS frames
  (`adts_count_bytes()`, tested) to log audio milliseconds received per
  wall milliseconds every 5 s, plus profile, core rate, channels, frame
  sizes, bytes lost hunting for a sync, and the real bitrate.
  **0039's run answered all three, and found the real problem:**
  - *The stream really is 512 kbit/s.* AAC-LC, 48 kHz stereo, frames
    1365-1366 bytes, 0 bytes lost. For an AM talk station, absurd; but
    real, and 64 KB/s is what it needs.
  - *The link could not deliver it.* 0.72x, 0.73x, 0.15x, 0.50x, 0.68x,
    0.63x, 0.66x, 0.68x, 0.28x of real time -- 30 s of audio in 64 s.
    The first probe's 433 kbit/s was also only 0.85x.
  - *ESP-Hosted's host ran out of buffers constantly:* `mempool OOM
    start (RX)` roughly every second, and twice `(TX)` then `(RX)`
    together with nothing for about ten seconds (79514-88525,
    120475-129998). Internal free was 46 KB with the session up; the
    minimum reached 38 KB.
  - *The token:* `"iat":1789097220,"exp":1789097280,"rttl":5` -- the
    redirected URL is good for 60 seconds. A reconnect inside that could
    reuse it and skip 0.8 s; after it, Zeno again.
  - *Nothing was playing.* The full log of the same boot (v0.3.0-83)
    has the player idle on "ready to resume" from boot until a track was
    started at 344952, three and a half minutes after the probe ended.
    So the shortfall and the OOMs are the network path alone, with no
    decoder, SD reads or writer competing for anything.
  **0040** puts mbedTLS and lwIP/Wi-Fi buffers in PSRAM
  (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`, `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`)
  on the hypothesis that they crowd the transport's buffers out of
  internal RAM, and the probe now logs internal heap every window.
  **The first flash of 0040 did not apply it:** the sdkconfig still read
  `CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y` and TRY_ALLOCATE unset, because
  `idf.py fullclean` -- which 0040 and this file both offered -- does not
  touch sdkconfig. `rm sdkconfig` is the only way. That run was not
  wasted: with a card track playing, internal free was 82 KB before the
  probe (98 KB idle), one TLS session took it to 35 KB with the largest
  internal block at 14 KB, and hop 2 failed with `esp-aes: Failed to
  allocate memory` then `HTTP -1`. So streaming beside playback does not
  fit in internal RAM as configured, whatever the throughput. Hardware
  AES wants DMA-capable internal copies of its buffers, which is worth
  remembering if the TLS buffers do move to PSRAM.
  **After `rm sdkconfig`, 0040 applied, and it was the fix for the
  stalls** (a card track playing throughout):
  - *Internal RAM:* 82.9 KB free before; hop 1 connected at 69.5 KB (13 KB
    taken, against 46 KB before), hop 2 at 60.9 KB, minimum 52 KB, largest
    block 31.7 KB the whole minute. PSRAM took the other 33 KB.
  - *No `eh_sdio: mempool OOM` at all,* in 60 s, against one a second and
    two ten-second stalls before. No `esp-aes` allocation failure either,
    with hardware AES still on.
  - *Throughput:* 1.02, 0.99, 0.93, 0.72, 1.01, 0.88, 1.02, 1.01, 0.95,
    0.85, 0.91x -- 56.3 s of audio in 60.2 s, 0.93x overall, 53-64 KB/s.
    From 0.47x idle to 0.93x with playback. Not yet enough: a 512
    kbit/s stream draining a buffer at 7% would run dry, and several
    five-second windows fell to 0.72-0.88x.
  - The hypothesis held: TLS and lwIP buffers in internal RAM were
    starving ESP-Hosted's. What remains is below 1.0x with no memory
    pressure showing, so the next suspect is lwIP's TCP window
    (IDF's defaults are small and nothing here raises them), then the
    SDIO link itself.
  **Idle on the same build:** 64.8 s of audio in 60.1 s, 1.08x; windows
  0.94, 1.15, 1.30, 1.13, 1.11, 1.10, 1.23, 1.14, 0.99, 0.90, 0.82x at up
  to 81 KB/s; internal minimum 67.8 KB; no OOMs. Windows above 1.0x are
  the server's backlog for a new listener arriving faster than live, so
  the link can carry more than 64 KB/s. Once it was drained the rate
  fell to 0.82-0.99x. So playback costs about 15%, and neither case holds
  1.0x steadily after the backlog.
  **External measurements, for scale:** a P4 + C6 over ESP-Hosted SDIO
  test measured about 36 Mbps UDP with the C6's single core as the
  limit (github.com/r4d10n/esp32p4-c6-wifi-test, slave firmware
  v2.11.7); an esp-hosted-mcu issue measured 7.5-13 Mbps TCP on the
  same pairing (issue #121). This stream needs 0.51 Mbps. The shortfall
  is configuration or the C6 firmware, not the hardware.
  **0044** takes the receive half of ESP-Hosted's throughput-tuned iperf
  host defaults: `LWIP_TCP_WND_DEFAULT=65534`, TCP and TCPIP receive
  mailboxes 64, `LWIP_TCP_SACK_OUT`. Not the watchdog, priority or
  IRAM-optimisation settings from the same file; see sdkconfig.defaults.
  If it does not reach a steady 1.0x beside playback, M5's 0.0.0 C6
  firmware ("CP without SDIO SW_AGGR; compatible streaming mode") is the
  next suspect.
  **0044 was worse, and 0045 takes it out.** Idle, hop 2 connected with
  internal free down from 86 KB to 44.5 KB, and 70 ms later
  `eh_sdio: dma_alloc(9216) failed; dropping read` and `rx_get_buffer(8736)
  failed; skipping read`, then nothing for ten seconds, `esp_tls_conn_read
  error`, and the probe's read failed: 618 ms of audio in 20 s, 0.06x.
  ESP-Hosted's SDIO receive path takes a DMA buffer from internal RAM for
  each read and holds it until lwIP takes the packet, so a 64 KB window
  and 64-deep mailboxes let the server's opening burst pile up in exactly
  the RAM the transport needs, and a dropped read inside a TLS stream is
  fatal to the connection. The receive-side knobs that would absorb bursts
  belong to the transport, not to lwIP: the esp-hosted-mcu main branch has
  `ESP_HOSTED_HOST_SDIO_RX_Q_SIZE` and `ESP_HOSTED_HOST_SDIO_RX_STAGING_SLOTS`,
  but whether 3.0.7 has them, and whether its pool can live in PSRAM
  (ESPHome's esp32_hosted component exposes a use_psram for it), is not
  checked -- grep managed_components/espressif__esp_hosted for Kconfig
  options before trying either.
  **Checked:** 3.0.7 has `ESP_HOSTED_HOST_SDIO_RX_Q_SIZE` (20),
  `ESP_HOSTED_HOST_SDIO_RX_STAGING_SLOTS` (2, range 2-8) and
  `ESP_HOSTED_DFLT_TASK_FROM_SPIRAM`, and no PSRAM placement for the SDIO
  pool. The first two buy burst headroom with more DMA RAM -- internal
  RAM here, the thing 0044 ran out of -- so they are not the next step.
  **0045 flashed, idle:** 63.3 s of audio in 60.1 s, 1.05x; windows 0.93,
  1.01, 0.96, 1.01, 0.91, 1.10, 0.87, 1.19, 1.22, 0.97, 1.31x; internal
  minimum 69 KB; no OOMs. The 0040-0043 behaviour is back. It also
  corrects a reading two entries up: the windows above 1.0x are not only
  the server's opening backlog. Here they came at 41-56 s, after dips --
  the link falls behind for a few seconds and catches up, so the server
  is pacing to real time and the average holds. What a stream player
  needs from that is a buffer of a few seconds that rides the dips, not
  a faster link. Beside playback, on this build, it was 0.93x.
  **Beside playback on 0045 (v0.3.0-90, a card track from 5871):** 58.5 s
  of audio in 60.2 s, 0.97x; windows 0.86, 0.89, 0.83, 1.06, 1.04, 0.93,
  0.91, 1.03, 1.03, 1.05, 1.05x; internal minimum 53 KB; no OOMs. So the
  settled numbers for WNZK's 512 kbit/s are **1.05x idle and 0.97x beside
  MP3 playback off the card**, with swings of 0.83-1.31x in 5 s windows.
  A stream player replaces the card track rather than running beside it,
  so its real figure sits between the two.
  **What the stream path takes from this:**
  - A pre-buffer before sound, and a buffer deep enough to ride
    five-second dips to 0.83x: 10 s of 512 kbit/s AAC is 640 KB, which
    is PSRAM's business, not internal RAM's.
  - A rebuffer policy for when it drains anyway -- at 0.97x it would,
    slowly -- that says so on screen instead of stuttering.
  - Internal RAM stays the constraint: one TLS session costs 13 KB even
    with mbedTLS in PSRAM, lwIP must not be given a large window
    (0044), and the free figure with a track playing and the stream up
    was 53-63 KB.
  - WNZK is an unusually heavy stream. Most stations send 64-128 kbit/s,
    a quarter or less, and have margin this link does not give WNZK.
  For scale, internal RAM at boot (heap_init): 256 KB main, 71 KB
  retention, 31 KB RTC, 18 KB and 7 KB more -- about 383 KB, of which
  about 98 KB is free once the player is up. Where the rest goes has not
  been audited.
- **The stream path itself.** Phase 1 is written (the 0100 series,
  below). Phases 2-4 are the plan, unchanged.

### The 0100 series: phase 1, written and not compiled

`netstream.c` exists, with its two pure pieces host-tested and itself
never put through a compiler. That split is deliberate and is the same
bargain `texttest` was built on: a build here is expensive enough not to
be the feedback loop, so the parts that can be tested on a host are
separated out until they are *most* of the logic, and what remains is
plumbing that only a flash can judge.

**0101, `icydemux.h`.** The probe's inline ICY loop, lifted out and made
a struct. The property is split-invariance -- the same body in 1-byte,
3-byte, 97-byte, 1000-byte and single-shot pieces must give identical
audio and identical titles -- plus 2000 random bodies in random pieces
against the same bodies whole. 4064 checks under ASan and UBSan.

It found a bug that would have been invisible on hardware and maddening
in use: **a metadata block with no StreamTitle blanked the title.**
`icy_stream_title()` terminates its output before it searches, so
passing `d->title` straight to it clears the title on every block that
carries only a `StreamUrl` -- which stations send. The screen would have
gone empty and come back for no reason a listener could see. It parses
into a scratch buffer now.

Worth keeping in mind for the next reader: metaint is 16000 and the
reads are 2048, so a length byte lands on a read boundary about one time
in eight. A wrong demultiplexer plays for a minute before it desyncs,
which is exactly long enough to be believed.

**0102, `netplan.h`.** Statuses, hops and backoff as a table, 1202
checks. 404 is fatal and 5xx retries; five redirects then fatal; 1, 2,
4, 8 seconds and then FAILED, a bounded fifteen seconds from first
failure to a screen that says something actionable.

Two things the probe had already taught, written as functions so a test
can fail rather than as comments:

- `netplan_reconnect_from()` always returns the station URL. The Zeno
  token lives 60 s, so a cached resolved URL **works for the first
  minute of testing and then never again** -- the worst shape a bug can
  have, and the reason this is not left to the reconnect path's
  discretion.
- `netplan_made_progress()` resets the failure count on audio, not on a
  successful connect, so a station that accepts a connection and drops
  it after 200 bytes is given up on rather than retried forever.

**0103, `netstream.c`.** The task, the 256 KB PSRAM ring (static buffer
over a permanent allocation -- `xStreamBufferCreateWithCaps()` is still
banned and this does not reintroduce it), redirects by hand, published
values and no handles across tasks. Requests are a generation counter
rather than a queue, because a second play request while one is
connecting means "that one instead", not "that one next".

Two judgements to disagree with later if a flash says so:

- **A full ring drops.** Stalling the read is how a server decides to
  disconnect us, and for a live stream a full ring means the server is
  ahead of real time. Dropping is the honest answer; a stall is a
  disconnect with extra steps.
- **A zero read is told from an ended body by time, not by count.** A
  live stream never ends on purpose, so ten seconds of silence is a
  drop. The probe's 50-count heuristic was fine for a 60-second probe
  and is not fine for an evening.

**What a flash would settle, in the order it would hit them.** None of
this is knowable from here:

1. **Whether it compiles.** The enum spellings and the
   `esp_http_client` call shapes have never been checked by anything.
   This is the cheap one -- no cable, no flash.
2. **Whether `ICY 200 OK` ever arrives.** `netplan_icy_status()` knows
   what the line means; whether `esp_http_client` hands it over or
   fails the request outright is a hardware question. Zeno answers
   HTTP, so this will not fire on the station being tested with, and
   will fire on the first old Shoutcast relay somebody adds.
3. **Whether priority 3 is right.** Chosen to sit below the writer (6)
   and ui_task (4) and above `media_task` (1), on the argument that the
   network must never delay the writer and the ring covers the gap. That
   is reasoning, not measurement.
4. **Whether the ring is ever full, or ever empty.** At 0.97x beside
   playback it should trend empty, which is phase 3's problem; if it
   trends full, the drop path above is running and the log says so.
5. **What a real drop looks like.** Every reconnect path here has been
   reasoned about and none has been seen. The failure classification is
   the part most likely to be wrong in a way the table cannot show.

**Not done, and next:** the probe still opens its own connection rather
than being rewritten on top of `netstream`, which the plan calls for and
which is the thing that would prove this file on hardware. That is 0105
and it should come before phase 2 -- decoding from a ring that has never
been shown to fill correctly is two unknowns at once.

*(0105 did it. The paragraph is kept because it is the reasoning that
produced the patch.)*

### 0105-0106: what got done with no board

The device was unavailable for flashing, which changes what is worth
writing. Both of these are chosen for that: one is the patch that will
be run first when a board comes back, and the other is the piece of the
plan that never needed one.

**0105, the probe on top of netstream.** The probe's job changed rather
than shrinking. It used to measure the network; it now measures
netstream, and the difference is entirely where the ADTS counter sits.
The old probe counted frames off the socket and reported **0 bytes lost
hunting for a sync at 512 kbit/s** -- a figure we have for this exact
station on this exact link. The new one counts them after the ICY
demultiplexer and after the ring, so the same station giving a different
answer is netstream losing bytes and nothing else. A desynchronised
demuxer, a ring dropping under burst and a reconnect that loses its
place all land in the same two numbers.

Beside that it logs what only the draining side can see: every state
transition with its timing, ring occupancy per window and its peak,
empty reads per window, and how long `netstream_stop_wait()` takes --
which is what phase 3's pause will cost.

**When a board is free, this is the first thing to flash, and the log
answers in this order:** does it compile; does the state reach PLAYING
and how long did it take against the old probe's 2.8 s to first audio;
is x-real-time still about 0.97x beside playback or has it fallen; is
`bytes lost hunting` still 0; does the ring sit near empty (healthy),
near full (netstream is dropping) or oscillate; and does the stop
complete in well under 5 s.

**0106, `stationlist.h`.** Phase 4's file format, out of order because
phases 2 and 3 are both "flash and read the log" and this is not. 3067
checks including 3000 random line-soup files, which must produce no
station whose URL fails the scheme check and none with an empty label.

It is the widest input in the whole network series. Everything else here
reads bytes from a server; this reads bytes from a person with a text
editor, so it takes a BOM, CRLF, bare pasted URLs, leftover `#EXT-X-`
directives, names containing commas and a missing trailing newline, and
refuses anything that is not `http://` or `https://` -- a `file://` line
is a path into the card walked by a stream player that has no business
there. Refusing at parse time means `netstream_play()` only ever sees
one of two schemes.

The bug it was written to prevent, and which has its own test: **an
`#EXTINF` whose URL line never came must not attach its name to the next
station.** Every station showing the previous station's name is the
shape of thing that ships.

**-Wformat-truncation, for the third time.** `snprintf(dst, sizeof dst,
"%s", src)` is correct at every call site in that file -- the length is
already checked -- but the compiler cannot see the check from the call,
and the diagnostic only exists after the constant propagation that -O2
does, so it is invisible at the -O1 the sanitiser build uses. The
Makefile's separate `-O2 -Werror` pass caught it before `idf.py` did,
which is exactly what that pass is for and the third time it has paid
for itself. The file has no `snprintf` in it now, which is also the
better shape: truncating a URL gives a station that connects to the
wrong thing, so the call sites reject instead.

**0108 built.** The one thing a build alone could settle is settled:
`netstream.c`, `icydemux.h`, `netplan.h` and the rewritten probe all
compile in the real IDF build at -Werror. The enum spellings and
`esp_http_client` call shapes 0103 flagged as its most likely errors
were not among them; the one error was a header that did not name its
own buffer size, which was not on the list and could not have been --
nothing had ever tried to call it from outside.

So item 1 of "what a flash would settle" is closed and items 2-5 are
untouched. Every remaining unknown in phase 1 needs a board.

**0110, `bufferplan.h`.** Phase 3's one question that is not a
measurement: given the PCM ring's fill level, does the writer run? A
file never asks -- the card is faster than playback and the ring is
always full -- and a live stream at 0.97x asks several times an evening.

Two thresholds and not one. `if (buffered < 1s) stop;` flaps at the
watermark, the writer stops and starts milliseconds apart, and that is
heard as chopping rather than as a pause. Stop below 1 s, do not start
again until 4 s, first sound at 4 s. **The numbers are guesses from the
probe's 0.83x five-second windows and a flash will move them; the
machine will not**, which is why they are constants in a tested file
rather than numbers in `player.c`.

The edges are the content. A stream that ends with eighteen seconds
still in the ring plays them out rather than going silent on the socket
close. A rebuffer whose source has given up drains instead of waiting
behind a "Buffering" message that is a lie. A station too slow for this
link gives up after 30 s rather than sitting on that message all
evening, and 30 s is deliberately longer than netplan's whole 15 s
backoff schedule so a source still working through its retries is never
cut off by it -- `bufferplantest` asserts that relationship rather than
leaving the two constants to drift.

20153 checks, including 20000 random walks asserting the invariants that
matter: never audible with an empty buffer, never a return to PLAYING
below RESUME_MS, ENDED is terminal, and audible and finished are never
both true.

The walks found a real one. `PLAYING` with a dead source only became
`DRAINING` once the level fell below LOW_MS, so the machine reported
"playing" for up to eighteen seconds after the stream was over. The
audible behaviour was identical either way -- both keep the writer
running -- which is exactly why it would have survived listening to it;
what was wrong was the phase, which is what the screen and the log read.
It enters `DRAINING` the moment no more bytes are coming.

**0111, `codecplan.h`.** Phase 2's one decision that is not a
measurement: which decoder the ring's bytes go to. There are two
sources and they disagree often -- Content-Type is a header somebody
configured years ago, and the first bytes are what the decoder will
actually be handed.

**The bytes win.** The header is consulted only when the bytes say
nothing. This matters because of the failure mode: handing AAC to
minimp3 does not error, it produces noise, and noise from a live stream
with no seek bar is very hard to tell from a bad connection. A single
station with a stale Content-Type would present as "internet radio is
flaky". An AAC station announcing `audio/mpeg` is routine, not
hypothetical.

Everything outside phase 2's scope -- ADTS AAC and MP3 -- is refused
**by name**. An Ogg stream, an HLS playlist and an error page are three
different things to say on a screen, and "failed" for all three is the
version that produces a bug report nobody can act on. The playlist cases
earn their place: a great many station URLs in the wild are `.pls` or
`.m3u` files that *contain* the stream URL, radio-browser hands them
out, and resolving them is explicitly not in scope -- so the least this
can do is say which kind it found rather than playing a text file as
audio.

Two things separated that a simpler version would merge:

- **"Not enough bytes yet" is not "not audio."** A station whose first
  block has not arrived says Buffering, not Unsupported.
- **ID3 is a wrapper, not a codec.** A Shoutcast MP3 stream can open
  with a tag; `id3_skip_bytes()` reads its length so the caller drops it
  and sniffs again. The length is four syncsafe bytes -- seven bits
  each -- and reading it as a plain big-endian integer is wrong
  silently, for every tag under 2 MB, landing mid-frame. Tested
  directly, and end to end: tag in front of real MP3, skip, re-sniff,
  decode.

8053 checks, including every combination of sniff result, Content-Type
and byte count, and 5000 random buffers through the real sniffer.

**0112: the first flash, and the first panic.** `netstream` died with a
stack protection fault before `netstream_init()` reached its own log
line -- which is how the log was read, since the absence of the
"ready: 256 KB ring" line placed the crash on the task's first pass and
ruled out everything to do with the network.

The backtrace pointed at `xSemaphoreGive(s_lock)` and
`xTaskPriorityDisinherit`, which had nothing to do with it. **A stack
protection fault names the line that was running when the floor was
crossed, not the line that consumed the stack.** The real cause was
`sizeof(icydemux_t) == 4392` -- the `meta[4081]` buffer that a
maximum-size ICY block requires -- sitting on the task stack as a local,
with 608 bytes of url and name and an inlined `url[512]` beside it. 6 KB
gone before the first read.

Worth noting against 0101's own reasoning: that patch made the
demultiplexer a struct precisely so it could be *owned, reset and
inspected* rather than living as locals in a read loop. It was right
about that and silent on where the struct should live, and the obvious
place turned out to be the one place it could not go.

The demuxer and both URL buffers are at module scope now, which is safe
for exactly the reason the ring is: one task, one stream, enforced by
`netstream_play()` replacing a running stream rather than starting a
second. The stack is 8 KB rather than 6 -- probably more than needed now
that 5 KB has left it, deliberately generous until the number has been
seen under a TLS session. It logs its own high-water mark every window,
so the right size will be an observation instead of a third guess.

**0113: `sizeof` on a pointer, and why the host pass did not catch it.**
0112 moved the demuxer and URL buffers to module scope and, to keep the
function body reading the same, aliased them back with
`char *const url = s_url;`. That silently changed every `sizeof(url)` in
the function from 512 to 4 -- the width of a pointer -- and every
station URL would have been truncated to three characters.

gcc refused it: *"up to 511 bytes into a region of size 4"*. That is the
good outcome and not a compiler quirk. The same mistake behind a
`memcpy` or a `strncpy` compiles, ships, and produces a URL that fails
to resolve for no visible reason. **`-Wformat-truncation` has now caught
two separate bugs in this series** (0106's was the opposite case, a
correct call the compiler could not prove), which is worth more than the
one false positive it cost.

The real gap is why the `WARNCFLAGS` pass missed it. **That pass only
covers the `texttest` translation units and `check-ui`.** `netstream.c`
is not compiled on the host at all, because it needs
`esp_http_client` and FreeRTOS, so the only thing that ever sees it is
`idf.py`. Every diagnostic in that file therefore costs a full IDF
build to discover, and both of 0112's and 0113's faults were of a kind a
host compile would have caught in a second.

Worth considering rather than assuming: stub headers good enough to
compile `netstream.c` on a host -- not to *run* it, just to get
`-O2 -Werror` over it. `texttest/fake` already does this for the UI. It
would not have caught the stack overflow, which is a runtime fact, but
it would have caught this one and the `ICY_TITLE_MAX` one in 0108.

### 0114: it streamed, and the throughput regressed

Phase 1 works end to end on hardware. Items 2 to 5 of "what a flash
would settle" are now answered, and one of the answers is bad.

**What went right, read in the order 0107 said to read it in.**

    hop 1: HTTP 302 -> redirect, connect+TLS  855 ms, metaint 0
    hop 2: HTTP 200 -> play,     connect+TLS 1634 ms, metaint 16000
      content-type: audio/aac   icy-name: WNZK-AM   icy-metaint: 16000
    first audio byte out of the ring at 3166 ms
    ADTS: 2238 frames, profile 2, 48000 Hz, 2 ch, 1365-1366 bytes,
          0 bytes lost hunting
    stop completed after 119 ms, state idle

The redirect walk, the ICY header capture, the state machine, the sniff
and the drain all behave. **0 bytes lost hunting for a sync, measured
after the demultiplexer and after the ring** -- the same figure the old
probe got off the socket, which is what 0105 was built to make
comparable. `icydemux` is correct on real traffic, including the metadata
block that arrives every 16000 bytes, and the title is `" - "` exactly
as predicted. Stop takes 119 ms, so phase 3's pause is cheap. No
reconnect happened, so item 5 is still unanswered.

**The stack, now measured instead of guessed.** Low water 4848 of 8192,
so the task's peak use is 3344 bytes with a TLS session open. 6 KB would
have fit *after* 0112 moved 5 KB off it -- which is worth noting
precisely because 6 KB was the number that panicked. It stays at 8192:
the measured path did not include a reconnect or a second redirect, and
2.8 KB of margin costs nothing that is in short supply. Internal free
bottomed at 46916 with a track playing, comfortably clear.

**The bad number.** Throughput is **0.79x overall**, windows 0.76-0.92,
mean 0.835. The link delivered a mean of 428 kbit/s against the
station's 511. The old probe, reading the same station off the socket on
the same link, measured **1.05x idle and 0.97x beside card playback.**

That is a regression of roughly 100 kbit/s and it must be explained
before either figure is trusted. Two differences between the runs, both
cheap to separate:

1. **This run played from USB, the old one from the card.** USB 2.0 MSC
   and the Wi-Fi SDIO link are the obvious contention, and the earlier
   0.97x figure was explicitly "beside *card* playback".
2. **The bytes now cross a task boundary and a ring.** That should cost
   almost nothing at 54 KB/s, but it has not been shown to cost nothing.

**Re-run wanted, before any of phase 2:** the probe beside SD playback,
and the probe with nothing playing. Those two readings against the old
probe's 1.05/0.97 say whether the ring costs anything or whether USB
does.

**The thing that stops this being a crisis.** The link delivers ~428
kbit/s fairly steadily whatever it is asked for, and WNZK at 511 kbit/s
is an outlier -- most stations are a quarter of that. The same 428
kbit/s is **3.3x real time for a 128 kbit/s station**, 2.2x at 192 and
1.7x at 256. So internet radio is comfortably viable and *this station*
is marginal on this link. A station list should probably not lead with
it, and phase 3's watermarks cannot be tuned against it: at 0.79x the
buffer drains forever and `bufferplan` would correctly give up after 30
seconds every time.

**One log line that looks wrong and is not.** `ring 0% (2047 bytes)`
appears in every window, always exactly 2047. That is the reader and the
writer in lockstep -- netstream adds 2048, the probe's next
`netstream_read()` takes 2048, and a single metadata byte removed early
leaves the odd one behind. A ring that hovers near empty with a reader
faster than the network is the correct picture, not a stall.

### 0115: the stream path was holding 41 KB of internal RAM

The second run answered item 5 -- what a real drop looks like -- by
causing one, and the cause was this code.

    W eh_sdio: dma_alloc(5120) failed; dropping read
    W eh_sdio: rx_get_buffer(4700) failed; skipping read
    E transport_base: esp_tls_conn_read error, errno=No more processes
    E esp-tls: getaddrinfo() returns 202          [for the rest of the run]

**The code that ran out of memory was not the code that took it.**
Nothing in netstream failed an allocation; the Wi-Fi transport failed a
DMA allocation, the socket died, and DNS stayed broken for every
subsequent attempt. Internal free was still reporting 58-60 KB at the
time, because **DMA-capable internal RAM is a subset of internal RAM and
runs out earlier than the free-heap figure suggests.**

What took it: `static uint8_t buf[2048]` is internal RAM, and that is
the default nobody thinks about. Adding up the demuxer (4392), two 2 KB
working buffers, the sniff buffer, an 8 KB stack, and the probe's own
two 2 KB buffers and 6 KB stack: **about 41 KB of the 53-63 KB free**,
against a file whose own header identifies internal RAM as the scarce
resource and rations TLS sessions to one. 0103 was careful about the 13
KB it could see and careless about the 26 KB it was itself.

15 KB of internal RAM handed back: the demuxer, all four working buffers
and the probe's two move to PSRAM (the bytes arrive by `memcpy` out of
mbedTLS, not by DMA, and 54 KB/s through PSRAM is nothing), and the
stack drops from 8192 to 6144 on the strength of the measurement -- the
high-water log said 4848 free of 8192 across a full minute with TLS open
and a redirect walked, a peak of 3344. Only the stack has to stay
internal.

**Two smaller things the same log exposed.**

`attempt 0 failed; retrying in 1000 ms` is not a thing. After a drop
that had played audio, `netplan_made_progress()` correctly reset the
count to 0 and the backoff was then indexed at `s_failures - 1` -- minus
one -- landing on `netplan_backoff_ms()`'s clamp. The right answer by
accident. The reset case is its own branch now and says what happened.

**The backoff schedule assumed attempts are cheap, and they are not.**
A DNS failure took 13590 ms against a 1000 ms backoff, so "1, 2, 4, 8
and give up after 15 seconds" was really four attempts of fourteen
seconds each -- a minute of retrying behind a screen promising fifteen.
The wait now subtracts what the attempt already cost, because the
backoff exists to stop hammering a server and an attempt that spent
longer than the wait has already done the waiting. `netplan`'s table is
unchanged and still right; what was wrong was assuming its numbers
dominated the loop.

Also: the body read timeout drops from 10 s to 5. The run spent twenty
seconds deciding the stream had stopped -- ten inside esp-tls reaching
its own timeout, then ten more here -- and only the second is ours.

**What this does not explain.** The throughput regression from 0114
stands untouched; the first window of this run was 0.35x before the
transport failed, which is consistent with memory pressure already
biting rather than with a slow link. So 0114's 0.79x may have the same
cause as this crash, which would be a happier answer than USB
contention. **The re-runs 0114 asked for are now more interesting, not
less:** with 15 KB of internal RAM back, beside SD and with nothing
playing.

### 0116: the throughput question, answered -- it is not the code

Third run, beside SD playback, after 0115. **0115's fixes hold and the
throughput question resolves, but not the way either candidate in 0114
predicted.**

    internal free at probe start   94423   (was 84259)
    internal minimum during run    57268   (was 46916, and 43560 when it starved)
    stack low water                 2784 free of 6144 -> peak 3360 bytes
    no dma_alloc failures, no drop, full 60 s, stop in 119 ms
    ADTS: 0 bytes lost hunting, again

12 KB more internal RAM at the start and 14 KB more at the worst moment.
The stack at 6144 peaks at 3360 with 2784 spare, matching the 8 KB
measurement almost exactly, so that size is settled.

**The number.** 0.828x overall, mean 449 kbit/s. Against the USB run's
0.794x and 428 kbit/s. So **USB contention was worth about 20 kbit/s --
real, but nowhere near the 100 kbit/s gap** -- and the extra 12 KB of
internal RAM bought nothing measurable either. Both of 0114's candidates
are wrong.

**What the window breakdown says instead.** The SD run's windows run
0.89, 0.97, 0.96, 0.90, 0.98, **1.01**, 0.80, **0.51**, 0.88, 0.82. Four
consecutive windows at 0.96-1.01, and a peak of 526 kbit/s. **The ring
and the task boundary sustain 1.03x of this station's bitrate when the
link allows it**, which is the question that actually mattered and the
answer is that the code is not the bottleneck. The mean is dragged down
by a floor that fell to 0.51x, not by a ceiling.

**And the comparison was never controlled.** RSSI was -40/-42 dBm on the
first run and -38 on the starved one; this run was **-47 dBm**, five to
seven dB worse, on a 2.4 GHz channel with twenty-four networks in
earshot. The old probe's 1.05x and 0.97x were measured on a better
signal, and the old probe no longer exists to re-run. So the honest
conclusion is not "throughput regressed" but **"nothing here measures
the link twice under the same conditions, and the code's ceiling is
fine."**

**Stop benchmarking on WNZK.** 511 kbit/s AAC sits right at this link's
capacity on a good day and below it on a bad one, which makes every
number ambiguous: a dip could be the link, the station, or the code, and
there is no headroom to tell them apart. At 449 kbit/s mean the same
link is 3.5x real time for a 128 kbit/s station and 2.3x at 192. **Phase
2 and 3 should be measured on an ordinary station**, with WNZK kept as
the deliberate worst case -- and `bufferplan` must not be tuned against
it, since at 0.83x its buffer drains forever and giving up after 30 s is
the correct behaviour rather than a bug.

**Items 1-5 are now all answered.** It compiles, it reaches PLAYING in
about 3.3 s, throughput is link-limited rather than code-limited,
the ring correctly trends empty behind a faster reader, and 0115's run
exercised a real drop and a real reconnect. Phase 1 is done being
guessed at.

### 0117: four runs, and the ring costs nothing

The idle run 0114 asked for. With the USB and SD runs beside it the
whole question closes.

    run         mean kbit/s   peak   x-real-time
    idle              500      596      0.926
    SD playback       449      526      0.828
    USB playback      428      468      0.794

**The playback penalty reproduces the old probe almost exactly.** Idle
to card playback costs **0.098x** here; the old probe, reading off the
socket with no ring and no second task, measured 1.05x idle and 0.97x
beside playback -- **0.080x**. Two measurements of the same quantity, a
fifth of a window apart, one before the ring existed and one through it.

So the curve has the same *shape* and sits about 0.12x lower across the
board. That is not something a ring does; a ring that cost 12% would
cost it under playback too, and the penalty would have grown rather than
stayed put. What shifts a whole curve uniformly is the RF environment,
which is exactly what the RSSI readings said -- **-38 to -47 dBm across
these runs, on 2.4 GHz with twenty-four to thirty-three networks in
earshot.**

**The ceiling settles it beyond argument.** A window reached 596 kbit/s,
**1.17x this station's bitrate**, through the demultiplexer, through the
ring, across the task boundary, with a reader on the far side. The path
is not the limit and never was. USB costs a further 0.034x, which is
real and small and worth remembering when someone plays from a stick.

**What the four runs actually bought.** Two genuine bugs -- the internal
RAM starvation and the reconnect bookkeeping, both found by running out
of memory -- and the retirement of a question that turned out to be
about weather. The measurement worth keeping from the whole exercise is
not any of the throughput figures; it is that **0 bytes were lost
hunting for a sync in all four runs**, which is `icydemux` and the ring
being exactly correct on two and a half hours of real traffic.

**The number phase 2 needs.** Internal free bottomed at **75100** on the
idle run and 57268 beside playback. That is the budget the AAC decoder
has to fit in, and it is a comfortable one -- the question 0116 called
phase 2's first is now answerable without fear. At 500 kbit/s mean this
link is 3.9x real time for a 128 kbit/s station, 2.6x at 192, 2.0x at
256.

### 0118: an ordinary station, and the measurement that would have vanished

0116 said to stop benchmarking on WNZK. The ordinary station is WUOM-FM,
128 kbit/s MP3 over StreamTheWorld -- about 3.9x of headroom at the
measured ~500 kbit/s, which is what phase 2 and 3 need.

**Pointing the probe at it would have silently stopped measuring.**
`streamsniff.h` counts ADTS frames and nothing else, because the only
station this had ever been aimed at was AAC. On an MP3 stream
`adts.frames` stays 0, `adts_ms()` returns 0, every window prints 0.00x
and the summary block does not appear at all. Not a failure -- an
absence. **The one figure four runs established, 0 bytes lost hunting
for a sync, is also the figure that certifies `icydemux` and the ring,
and it would have quietly become unavailable exactly when the station
changed.**

`mp3count.h` is the counterpart, the same shape as `adts_count_t`. The
probe feeds both counters every byte rather than choosing from the sniff
result, because choosing means not counting the bytes that arrive before
the sniff completes, and they cost a few comparisons each at 60 KB/s.

**Why MP3 is harder than ADTS, and what the test does about it.** An
ADTS header carries its own frame length. An MP3 header does not: the
length is computed from bitrate, sample rate and the padding bit, and
the arithmetic differs between MPEG1 and MPEG2/2.5. So a wrong table
entry does not produce a rejected frame -- **it produces a frame of the
wrong length, the stream desynchronises, and the damage appears as bytes
lost hunting.** A bad table would frame the ring for a fault it did not
commit. `mp3counttest` therefore computes all 756 frame lengths a second
time from the formula written out longhand, rather than checking the
tables against themselves.

UBSan found one in the first run: the sample-rate index can be 3, the
reserved value, and the code checked the *value* after using the index
on a three-element row. On the device that reads whatever follows the
table and computes a frame length from it.

Sync is also weaker -- eleven bits against ADTS's twelve plus a layer
field -- so random data does parse as occasional frames. The test pins
that at under 25% and it measures 7.9%, which is why **"bytes lost" is
the signal and "frames found" is not.**

**One thing to watch in the first log.** The station URL carries a
`uuid` parameter that looks like a session token. Zeno's redirect token
expired in sixty seconds and is why netplan always reconnects from the
station URL -- but here the token is *in the station URL itself*, so if
it is short-lived a reconnect will fail on it even though the policy is
right. A reconnect getting a 4xx where the first attempt got 200 is
that, and it is the station's constraint rather than a bug.

### 0119: the counter selector, and what the WUOM run actually said

The first MP3 run printed `18446744073709544819 ms of audio` and
`3343012699113726.04x`. Both are one bug, and the station turned out to
be something other than advertised.

**The bug.** The probe chose its clock per window with
`adts.frames ? adts_ms() : mp3_ms()`. The ADTS counter **false-positives
on MP3 data** -- the run gave it 93 "frames" at 88200 Hz with 0
channels, frame lengths of 25 to 8187 bytes and 442947 bytes lost. Junk,
but not *zero*, so the selector picked ADTS on an MP3 station. And
because it re-decided every window, the audio clock jumped between two
unrelated counters, went backwards, and the unsigned subtraction wrapped
to 2^64.

**`codecplan.h` was written for exactly this decision, before this code,
and then not used here.** 0111 built the table, argued that the bytes
must beat the header, tested it 8053 ways -- and the first caller that
needed it reached for `frames != 0` instead. A decision having a tested
home does not mean the next caller will find it.

**What the regression test found is more interesting than the bug.** The
obvious test is "the wrong counter loses most of its bytes" -- 54% on
the real station. On a synthetic MP3 stream it loses **4.5%**, because a
bogus frame length makes the ADTS counter *skip* a large block and
skipped bytes count as a frame body rather than as lost. That is what
"frame 25-8187 bytes" was. **How wrong the wrong counter looks depends
on what the audio happens to contain**, so no selector can be built on
those figures; the decision has to come from the sniffer.

**And the station is 64 kbit/s, not 128.** `icy-br: 64`, `icy-genre:
Talk`, and netstream settled at 61-65 kbit/s after an opening burst of
395 and 287. **StreamTheWorld bursts to fill a buffer and then paces at
real time.** That makes this station useless for measuring link
headroom -- x-real-time cannot exceed about 1.0 when the server will not
send faster -- and *ideal* for phases 2 and 3, because it behaves the way
a radio station behaves rather than the way a file server does.

So the two stations do different jobs and both stay: **WNZK for link
headroom, WUOM for behaviour.** Neither alone answers both. Also: first
byte at 2468 ms against WNZK's 3289, there being no redirect hop, and
the stop took 59 ms.

### 0121: a full ring waits, because WUOM front-loads 43 seconds

The clean WUOM run measured something that had not come up on WNZK, and
it condemns one of 0103's two named judgements.

**StreamTheWorld front-loads and then paces exactly.** Windows of 3.18x,
6.06x and 2.25x for the first fifteen seconds -- 58 seconds of audio --
then eight windows averaging **1.000x**. The server hands over about 43
seconds of audio up front and thereafter sends in real time.

0103 wrote: *"A full ring drops the bytes that do not fit rather than
stalling the read... for a live stream a full ring means the server is
ahead of real time."* It listed that as a judgement to disagree with
later if a flash said so. **The flash says so.** At 64 kbit/s the burst
is **454 KB against a 256 KB ring**, so a decoder draining at 1x would
have made netstream discard about 198 KB -- roughly **25 seconds of
audio**, a hole in the middle of a programme from a station that did
nothing wrong.

Both halves of the original argument are wrong. "The server is ahead of
real time" is the premise for keeping the bytes, not for throwing them
away -- being ahead is exactly what a buffer is for. And "stalling the
read is how a server decides to disconnect us" is not how HTTP works:
not reading closes the TCP window, the server stops sending, and the
surplus waits in its buffer. That is what every other streaming client
does.

So the send loop waits, in slices, still checking for a stop request
each time, and logs how long it has been waiting. A server that does
disconnect an idle reader exists -- and netplan already handles a drop
by reconnecting, which costs one reconnect against a guaranteed hole.

**This is why the probe drained as fast as it could and never saw it.**
The probe reads flat out, so the ring sat at 0% for all five runs and
the drop path never fired. It only appears with a decoder draining at
1x, which is phase 2. The bug was measurable a run before it was
reachable, and only because the probe logs the ring and the audio clock
separately.

**A number for phase 2 and 3:** the compressed ring will now stall
rather than overflow on this station, and `stalled ms` in the window log
is the figure that says whether 256 KB should grow. A ring big enough
for a 43-second burst at 64 kbit/s would be 344 KB; big enough at
128 kbit/s, 688 KB. Growing it is cheap in PSRAM, but stalling is not a
fault, so the number to watch is whether stalls cost reconnects.

### 0122: 0121 is unexercised, and one constant was doing two jobs

The run after 0121 reports **`stalled 0 ms` in every window**, which is
the predicted result and not a reassuring one. **The probe drains flat
out, so the ring never fills, so the path 0121 rewrote never executes.**
0121 is a reasoned change backed by a measurement of the *server*, not a
verified one -- it cannot be exercised until a decoder drains at 1x,
which is phase 2. Recorded here so that nobody later reads five green
runs as evidence for it.

What the run did surface is stop latency. Across six runs: 59, 59, 119,
119, 219 and **599 ms**. A stop is noticed between reads and not during
one, so the bound is the socket read timeout -- which was the same
constant as the drop timeout, at 5 s. **Phase 3's pause is a stop, and
five seconds of a button doing nothing is not a pause.**

They are split. `SOCKET_TIMEOUT_MS` is 1 s and bounds responsiveness;
`DROP_SILENCE_MS` stays 5 s and is measured from the last byte that
actually arrived, which is a property of the stream rather than of any
one read. The read can be impatient while the diagnosis stays patient.
They were ever the same number only because the first version had one
place to put it.

A paced 64 kbit/s station delivers 2048 bytes every 256 ms, so normal
stop latency was never the problem; the tail was. The figure to watch in
phase 3 is the worst case, not the median.

### 0123: 0122 broke connecting, and the retry path proved itself doing it

The run after 0122 took **18079 ms to reach first audio** instead of
2145, after four failed connections:

    W HTTP_CLIENT: Connection timed out before data was ready!
    I hop 1: HTTP -1 -> retry, connect+TLS 922 ms, metaint 0

**`esp_http_client`'s `timeout_ms` covers the header fetch, not only
body reads.** 0122 set it to 1 s to bound stop latency; this server takes
1.4 to 2 seconds between the TLS handshake and its first header, which
was comfortable inside the old 5 s and hopeless against 1 s. The fifth
attempt succeeded only because it happened to be quick, which is the
worst kind of pass -- the same change on a slower day fails outright.

Fixed by opening patient and becoming impatient afterwards: the client
is created with `CONNECT_TIMEOUT_MS` of 5 s, and
`esp_http_client_set_timeout_ms()` drops it to 1 s once the headers are
in and the only thing left is the body. Connecting is slow and happens
once a stream; reading is fast and constant. They want opposite
settings, and the API allows both -- 0122 assumed one knob where there
were two.

**The retry path proved itself while doing it**, which is the compensation:

    attempt 1 failed after 1928 ms; that is longer than the 1000 ms backoff, retrying now
    attempt 2 failed after 1941 ms; retrying in 59 ms
    attempt 3 failed after 1947 ms; retrying in 2053 ms
    attempt 4 failed after 1996 ms; retrying in 6004 ms

That is **0115's attempt-cost subtraction working exactly as argued**, on
the first occasion it has ever run: each wait is the backoff minus what
the attempt already spent, so the schedule means what it says instead of
being four fourteen-second attempts behind a fifteen-second promise. The
counter is honest, the messages are readable, and `netplan_action(-1)`
correctly classified an unparsed status as RETRY rather than FATAL. Had
the fifth attempt failed, `netplan_backoff_ms(4)` would have returned -1
and the state would have gone to FAILED -- correct, and untested until
now.

Also fixed: `bytes played; failure count reset` was printed after the
stop, because `pump()` returns on a stop request too. It read as though
a reconnect were being prepared for a stream that was ending.

**The lesson worth keeping.** 0122 was a reasonable change that improved
a real number, was checked against the suite, and broke the feature. The
thing it got wrong was not in the diff -- it was an assumption about
what a library constant covers. There was no way to catch it except by
running it, which is an argument for flashing small changes rather than
for reviewing them harder.

### 0124: phase 2 begins with the window, not the decoder

Phase 2 decodes from the ring. The first piece is not a decoder: it is
the window between the ring and one, because `mp3dec_decode_frame()` and
`esp_audio_simple_dec_process()` both want a contiguous buffer, both
report how much they consumed, and both may consume **nothing** because
the buffer does not yet hold a whole frame. Refill, partial consume,
slide along -- and that bookkeeping is where a decode loop goes wrong.

**The failure it prevents is not a crash.** A window that loses a few
bytes at each refill boundary desynchronises the decoder, which
resynchronises on the next frame header and carries on. The result is a
click every few seconds and a stream that otherwise works. On a live
stream, with no file to compare against and no seek bar to replay a
passage, that is close to undiagnosable from outside -- it presents as
"internet radio is a bit crackly".

So the invariant is asserted directly: **every byte written in comes out
exactly once, in order.** 2076 checks, including 400 real WUOM frames
(MPEG1 L3, 64 kbit/s, 208/209 bytes, the frame the hardware actually
receives) pushed through at chunk sizes from 1 byte to 16 KB, and 2000
random walks where the consume amounts ignore frame boundaries entirely.

Two decisions worth naming:

- **Overruns are refused, not clamped.** A decoder reporting that it
  consumed more than it was shown is a decoder whose return value has
  been misread, and clamping would slide the window past bytes nothing
  ever saw -- which is the click bug, arrived at politely.
- **The window must exceed the largest frame, or the loop hangs.** A
  frame that never fits means the decoder consumes nothing forever. ADTS
  carries a 13-bit length field, so the bound is 8191 bytes, and a
  window has to hold one whole frame plus the partial one before it.
  Twice 8191 is 16382 -- **a 16 KB window clears that by two bytes**,
  which is luck rather than design, so it is 24 KB and the test asserts
  4 KB of margin rather than a bare inequality. `framewin_stuck()` is
  the runtime answer for a stream that is simply not what it claimed.
  **160 KB since 0500**, by the same arithmetic against an Ogg page's
  65307 bytes; the reasoning above is unchanged and only the format with
  the widest frame is.

Nothing calls it yet. The decoder glue is next, and it wants minimp3's
frame decoder rather than `mp3dec_ex` -- **MP3 first rather than AAC**,
against the plan's order, because the benchmark station is now MP3 and
that is the path that can be watched. `codecplan.h` already routes both.

### 0125: netdec.c -- MP3 out of the ring

The decoder glue. `framewin` feeds `mp3dec_decode_frame()` and PCM comes
out.

**Why it is not `decoder.c`.** That file is built around a file: it opens
a path, builds or loads a seek index, answers duration and seek, and its
MP3 backend is `mp3dec_ex`, which wants a seekable source and an index
over the whole stream. A live stream has no path, no length, no index
and no seek. Threading one through that facade means teaching every one
of those to say "not applicable", and the result is a file decoder with
a stream-shaped hole in it.

What *is* shared is the decoder. minimp3 has two doors: `mp3dec_ex_*`,
the indexed file API, and `mp3dec_decode_frame()`, which takes a buffer
and reports how many bytes of it were a frame -- **already a stream
interface**. Same library, same vendored copy, same renamed symbols,
entered differently. `MINIMP3_IMPLEMENTATION` stays in `decoder.c` and
this links against it; `minimp3_prefix.h` comes first here too, for the
reason it exists at all.

**To be clear about a thing that sounds like a hardware constraint and
is not:** the P4 has no audio decoder in silicon. Every format this
player handles, files included, is decoded in software. minimp3 is
chosen over `esp_audio_codec`'s MP3 for the reasons in `decoder.h` --
Layers I and II, free format, Xing/LAME -- and its frame API is chosen
over its file API because a stream cannot seek.

**MP3 only, against the plan's order.** The plan said ADTS AAC first,
when WNZK was the station under test. The benchmark is WUOM now, which
is MP3, and the path that can be watched on hardware is worth more than
the one written down first. AAC is refused cleanly rather than
half-attempted: handing ADTS to minimp3 produces noise, not an error,
and noise from a live stream is hard to tell from a bad link.

Three things carried over from earlier mistakes rather than rediscovered:

- **The codec is identified once, by `codecplan_choose()`.** 0119 decided
  per window from whichever counter was non-zero, chose ADTS on an MP3
  stream and wrapped a clock. Decided once, then kept.
- **Everything is in PSRAM.** 0115 is the reason: internal RAM is what
  the Wi-Fi transport needs for DMA, and this path starved it once.
- **`frame_bytes` and the return value are different questions.**
  Consumed bytes and produced samples differ for an ID3 tag or junk
  before the first sync -- consumed, no samples -- which is a resync and
  not an error. A decoder reporting more consumed than it was shown is
  fatal rather than clamped, because a wrong window position *is* the
  click-every-few-seconds bug.

**Checked with stub headers before shipping**, which 0113 suggested and
nothing had done. `netdec.c` compiles at `-O2 -Werror` against a
throwaway `minimp3.h`, `esp_log.h` and `esp_heap_caps.h` in /tmp -- which
also checks the format strings and the `netstream.h` calls. It is not a
committed harness and it proves nothing about behaviour, but two of the
last four patches broke the build in a file only `idf.py` ever compiles,
and this one did not.

Nothing calls it yet: no task drives it, so the ring still has no reader
but the probe. That is the next patch, and it is the one that finally
exercises 0121's backpressure.

### 0126: the probe decodes, at real time

The patch that finally puts a reader on the ring, and the first one that
can fail for a reason phase 2 owns.

**Why pacing is the whole point.** Six runs reported `stalled 0 ms` and a
ring at 0%, because the probe drained flat out. A reader that goes as
fast as it can never lets a ring fill, so **0121's backpressure has never
once executed.** A real decoder consumes one second of audio per second.
WUOM front-loads about 43 seconds against a 33-second ring, so a paced
reader should fill it inside the first fifteen seconds and hold netstream
in its send loop. If it does not, something in 0121 is wrong.

So `decode_paced()` sleeps to keep decoded time level with wall time and
reports the two against each other. It is a writer that writes nowhere --
no I2S, no resampling, no volume. Those are phase 3's, and putting them
here would let this fail for reasons that are not the stream path's.

**What it certifies.** The raw mode's headline was "0 bytes lost hunting
for a sync", six runs running. The decode mode's equivalent is
`netdec_resyncs()`: the same claim -- the bytes reaching the decoder are
the bytes the station sent, in order -- measured on the far side of one
more component. It also prints, in as many words, whether the ring ever
approached full, so the answer to "was 0121 exercised" is in the log
rather than inferred from a peak figure.

**The raw mode is kept**, behind `STREAMPROBE_DECODE`. It produced the
six clean runs, and it is the mode that can still be pointed at a codec
`netdec` does not handle -- which is every codec except MP3 right now.
Both paths were stub-compiled at `-O2 -Werror` before shipping, the
`#else` branch included, because a branch nothing builds is a branch
that rots.

**What to read in the first log, in order:** does `first PCM` appear at
all; does the per-window ratio sit near 1.00x once the burst is absorbed;
does the ring climb past 90% in the first fifteen seconds and netstream
start reporting a non-zero `stalled`; and is `resyncs` zero. A non-zero
resync count on a station that gave six runs of zero bytes lost is
`framewin` or `netdec`, not the network.

### 0127: the second stack fault, and the opposite of the first

0126 drove `netdec` for the first time and it panicked on the first
frame:

    Detected in task "probe" at tab5_mp3dec_decode_frame
    Stack pointer: 0x4ff7b2f0
    Stack bounds: 0x4ff7e044 - 0x4ff7f840

SP is **11604 bytes below the floor**. `mp3dec_decode_frame()` puts its
scratch on the caller's stack -- that is minimp3's design and there is no
option to heap it -- and the probe task had 6144.

**This is the exact inverse of 0112.** There, a 4392-byte struct of ours
was on a task stack and the fix was to move it to PSRAM. Here **nothing
of ours is on the stack at all**: the window and the decoder state are
both already in PSRAM, precisely because of 0112. The space is consumed
entirely inside a vendored library, so there is nothing to move and the
only available fix is to give the caller a bigger stack.

The rule 0112 added -- check `sizeof` before putting a struct on a stack
-- would not have caught it. The generalisation that would have is: **ask
what a library puts on your stack before calling it from a task you
sized for your own code.** Added to the conventions.

The information existed. `media_task` has been 16384 since long before
any of this, for the same decoder on the file path. But the number lived
in an `xTaskCreate` argument and nowhere a second caller would ever look,
so it is `NETDEC_MIN_STACK` in `netdec.h` now, cited from both call
sites, and `netdec_open()` measures the calling task's headroom against
it rather than waiting for a panic. A panic names
`mp3dec_decode_frame` and a line inside a vendored header, which points
at the library rather than at the caller that is actually wrong.

**What the log did confirm before dying.** `netdec: ready: 24576 byte
window + 6668 byte decoder in PSRAM`, then `stream is MP3` --
`codecplan_choose()` identified the codec correctly on the first 64 bytes
of real data, and `sizeof(mp3dec_t)` is 6668 bytes, which is worth
knowing: it is large enough that having it on the stack too would have
been a second bug.

### 0128: the same fault twice, because the first fix read the wrong number

0127 raised the probe's stack from 6144 to 16384 and it panicked again,
at the same instruction.

    stack  6140 bytes, SP 11604 below the floor -> 17744 used
    stack 16380 bytes, SP  1364 below the floor -> 17744 used

**Two panics, identical total.** A stack protection fault reports where
SP had got to, which is the *overshoot*, not the demand -- and 0127 read
11604 as the requirement. The number that matters is
`bounds_size + (floor - SP)`, and the first panic could not show it
because there was only one of them. Two at different stack sizes give it
directly, and on a deterministic path they agree to the byte.

**The wrong precedent, too.** 0127 cited `media_task` at 16384 as the
task that decodes files. It is not: `play_file()` calls `decoder_read()`
and runs on the **main task**, which `sdkconfig` gives
`CONFIG_ESP_MAIN_TASK_STACK_SIZE=24576`. Had 0127 checked which task
actually calls the decoder rather than which task sounded like it
should, the number would have been right the first time -- **and it was
sitting in `sdkconfig` the whole time.** 24576 leaves 6.8 KB over the
measured 17744.

`NETDEC_STACK_FLOOR` is 20000 rather than something just above 17744, so
that a task which merely looks generous is still refused. 16384 looked
generous.

The refusal in `netdec_open()` was in 0127 and would not have helped:
the floor was 13000, which 16384 passes. **A guard derived from a wrong
measurement is a guard that confirms the wrong measurement.** It has the
measured figure now.

### 0129: phase 2 works, and the stall counter was measuring the wrong thing

The first run that decoded:

    first frame: MPEG layer 3, 44100 Hz, 1 ch, 64 kbit/s, 208 bytes -> 1152 samples
    2220 frames, 57991 ms of audio in 60023 ms, 0 bytes resynced
    ring peak 100% -- 0121's backpressure was exercised

**Everything phase 2 was supposed to prove, proved.** `framewin` and
`netdec` delivered 2220 frames with **zero resyncs** -- the same claim
the raw probe made six times as "0 bytes lost hunting", now measured on
the far side of the window and the decoder. The frame matches the
station exactly: 208 bytes, 1152 samples, 64 kbit/s mono at 44.1 kHz.
Steady-state windows decoded 5015 ms of audio in 5015-5016 ms, which is
**0.9998x** -- the tick-granularity worry did not materialise. The 0.966x
overall is the first window's startup burst and nothing else.

**0121 was exercised and is correct.** The ring climbed 40% -> 76% ->
99% in fifteen seconds, as predicted from WUOM's 43-second front-load
against a 33-second ring, and sat pinned at 99% for forty-five seconds.
Nothing was dropped. Netstream's input rate fell 408 -> 162 -> 64
kbit/s, settling at exactly the drain rate: TCP's window closing and the
server pacing itself to the reader, which is the behaviour 0121 argued
for against the original drop-on-full.

**And `stalled 0 ms` throughout, which is a flaw in the instrument.** The
counter only incremented when `xStreamBufferSend()` returned 0 after its
full 100 ms timeout. A decoder draining at real time frees a 208-byte
frame every 26 ms, so a send blocks briefly and partially succeeds and
never times out. It now measures **elapsed time in the send loop**,
whatever the shape of the waiting.

Worth being clear that the ring occupancy is what actually proved 0121,
not the counter that was added for the purpose. **A figure added to
measure a specific behaviour reported zero while that behaviour was
happening continuously**, and only the independently-logged ring level
contradicted it. Two unrelated measurements of the same thing is what
made that visible.

**The cost, for phase 3's budget.** Internal free bottomed at 56084,
against 79728 on the last raw run. The difference is almost exactly the
probe task's stack going from 6144 to 24576 -- **the decode task's stack
is now the largest single internal-RAM consumer in the stream path**,
and it cannot move to PSRAM. Phase 3 pays this once, on whichever task
decodes; it does not pay it twice.

### 0130: AAC, and the buffer that would have overrun

ADTS AAC through `esp_audio_simple_dec`, which completes phase 2's codec
scope. Same two backends and the same division of labour as the file
path: minimp3 for MP3, the codec component for everything else.

**A sizing bug caught before it ran.** `NETDEC_MAX_INT16` was `1152 * 2`
-- minimp3's worst case, and correct while MP3 was the only codec.
AAC-LC is 1024 samples a frame, but **HE-AAC doubles the output rate
through SBR**, so a stereo frame is 2048 * 2 = 4096 int16. WNZK, the
station this was written for, announces `audio/aacp` with a 48 kHz core,
which is exactly that case. **The first AAC frame would have overrun the
PCM buffer by 1792 int16.** The constant now matches
`DECODER_MAX_INT16`, which the file path already sized for the worst
case across both backends -- one number, sized once, for what is the same
decoder.

Worth noting how close that came to shipping: `netdec_read()` checks
`produced > max_int16` and would have returned -1 rather than writing
past the end, so it would have surfaced as "AAC does not work" rather
than as corruption. The check earned itself before the code it guards
was ever run.

**`use_frame_dec = false`** is what makes this work at all. It lets the
decoder's own parser find ADTS boundaries in whatever the window hands
it; `true` means "this buffer is exactly one frame", which a sliding
window cannot promise. That is also why `_ALAC`, `_VORBIS`, `_RAW_OPUS`,
`_ADPCM` and `_LC3` are unreachable from here -- they require
frame-at-a-time input, as `decoder.c` has noted since long before any of
this.

**Registration has one owner now.** `esp_audio_dec_register_default()`
was called behind a static flag inside `decoder.c`, invisible to
`netdec`. Two files with two private flags is precisely how a double
registration happens, so it is `decoder_register_codecs()` in
`decoder.h`, called from both.

**A reconnect closes and reopens the AAC decoder** rather than carrying
it over. It holds parser state for a body that has ended, and an ADTS
stream resumed mid-frame is the one thing that parser cannot be told
about. `eos` is never set true on the input: a live stream has no end to
signal, and claiming one would make the decoder flush and stop.

24-bit and 32-bit are refused rather than folded. The file path folds 24
and refuses 32 for good reasons; no broadcast AAC is either, so carrying
that machinery into a path that would never exercise it would be
untested code guarding an impossible case.

**To test it, point `STREAMPROBE_URL` at the commented WNZK line.** That
station is 511 kbit/s HE-AAC and sits at the link's capacity, so expect
the ring to behave nothing like WUOM's -- it will not front-load, and
0121's backpressure should not fire at all.

### 0131: AAC works, and 0130 claimed an overrun that would not have happened

    first frame: AAC, 48000 Hz, 2 ch, 16-bit, 1366 bytes -> 1024 samples
    decoded 2676 frames, 57088 ms of audio in 60016 ms, 0 bytes resynced
    ring peak 29% -- 0121's backpressure was NOT exercised

**AAC decodes, with zero resyncs**, same as MP3. And the prediction in
0130 held exactly: WNZK does not front-load, the ring stayed under 29%,
`stalled 0 ms`, and backpressure correctly never fired. A prediction
written before the run and confirmed by it is worth more than the run
alone.

**The correction.** 0130 said the old `NETDEC_MAX_INT16` of 2304 "would
have been overrun by 1792 int16" because WNZK is HE-AAC. It is not.
1366 bytes decode to **1024 samples** -- plain AAC-LC -- so a stereo
frame is 2048 int16 and the old buffer would have held it. The claim
came from an `audio/aacp` Content-Type seen in an early probe and was
never checked against a decoded frame; this run's header says
`audio/aac`.

The resize is still right -- HE-AAC streams exist and would produce 4096
-- but **the reason given was wrong, and the station that would actually
overrun has not been found.** Corrected in `netdec.h` too, since that
comment is what the next reader will believe.

**The number that matters more than either.** Internal free bottomed at
**40268**, against the MP3 run's 56084. The AAC decoder costs about
15.8 KB of internal RAM, and it allocates where it likes -- it is not
ours. For scale: **0115's transport starvation happened on a run whose
minimum was 43560.** This run went lower than that and survived, but not
by much, and largest-free-block held at 31744 throughout, which is
probably why.

So the AAC path is the tightest configuration the stream has ever run
in, and phase 3 adds an I2S writer and its buffers on top. `netdec` now
logs internal free either side of the decoder open so the cost is
attributed to a line rather than inferred by subtracting two runs.
**This is the thing to watch in phase 3, not throughput.**

Throughput itself: 0.951x overall, windows 0.89 to 1.17, mean 1.006.
That is the link at its limit on a 511 kbit/s station, exactly as 0116
described, and it is why WNZK is the worst case rather than the
benchmark.

### Where the stream path stands

Rewritten rather than appended to, because the previous version of this
paragraph had been edited four times in place and the tail of it was
still text from before the first flash -- it claimed phases 2 and 3
"both want a board before they are worth starting" and that phase 2's
first question was an unanswered measurement, several sessions after the
board answered it. **Four patches each replaced the sentence they
disagreed with and left the rest standing**, which is the same failure
1002 found in the open list and the reason that audit is a recurring job
rather than a one-off.

- **Phase 1: done.** Written, compiled, flashed five times, measured on
  two stations, two real faults found and fixed (internal RAM
  starvation, reconnect bookkeeping). Nothing about it is still a guess.
- **Phase 2: done.** MP3 and AAC both decode on hardware with 0 resyncs
  -- 2220 and 2676 frames. Internal free bottoms at **56 KB on MP3 and
  40 KB on AAC**, the latter being the tightest the stream path has run.
- **Phase 3: done and flashed** by the 0200 series. `play_stream()` is
  in player.c beside `play_file()`. See "The 0200 series" below.
- **Phase 4: done and flashed**, one bug outstanding. `stations.m3u`,
  `stations.c`, a RADIO tab in the chooser.

**Corrected, and it is the sentence this entry used to end with:** the
constants want tuning **against WNZK, never against WUOM**. This had it
exactly backwards. WUOM front-loads about thirty seconds into the
compressed ring and makes every watermark look free; WNZK paces at 1.00x
and never offers a surplus, so it is the station a rebuffer actually
costs something on. Measured on the board, both of them, in the 0200
series.

### The stream path: plan

Written after the probe, from what it measured. Each phase is its own
patch series, flashed and read before the next starts.

**The shape.** Three stages, two of which already exist:

    netstream task --> compressed ring --> decode loop --> PCM ring --> writer
    (new: HTTP, TLS,   (new, PSRAM,        (play_stream,     (exists)    (exists)
     redirects, ICY)    small)              new, in player.c)

The PCM ring is the important reuse. It is PCM_RING_BYTES, about twenty
seconds at 44.1/16/2 and eighteen at 48 kHz, and it lives in PSRAM. A
stream decoded as fast as the network delivers fills it ahead of the
writer exactly as a file does, so it already is the stream's jitter
buffer: the watermarks below are its fill level, not a second big
buffer. The compressed ring only decouples TLS reads from decoding and
absorbs a few seconds of burst -- 256 KB is four seconds of WNZK.

**Phase 1 -- netstream, no audio.** `netstream.c`: its own task, one
connection at a time. Open the station URL, follow redirects by hand
(Zeno's token lives 60 s, so every reconnect starts from the station URL,
never a stored redirect), strip ICY metadata into titles, write audio
bytes to the compressed ring, and reconnect on error or EOF with backoff
(1, 2, 4, 8 s, then give up and say so). States published as values: idle,
connecting, buffering, playing, retrying, failed. Pure and host-tested:
the ICY demultiplexer as a byte-at-a-time state machine (the probe's
inline loop, lifted out), the redirect/reconnect decisions, and the
backoff schedule. The probe is rewritten on top of it and still logs
KB/s and x-real-time, so phase 1 is proven on hardware before any audio.

**Phase 2 -- decode from the ring.** A decoder backend that reads a
callback instead of a FILE*. ADTS AAC through esp_audio_simple_dec, as
decoder.c already does for .aac files (its esp_codec path reads into
`inbuf` with storage_io_fread today; the stream variant blocks on the
compressed ring with a timeout). MP3 through minimp3's frame decoder, not
mp3dec_ex, which needs seeking. The first bytes, sniffed by
`streamsniff.h`, choose the codec; Content-Type is a hint, not an
answer. Measure what the AAC decoder costs in internal RAM before
anything else -- that is the resource with no slack.

**Phase 3 -- play_stream() in player.c.** Beside play_file(), not inside
it: a stream has no length, no seek, no sidecar, no gapless, no tail rules
and no measuring pass, and folding "unless it is a stream" into each of
those would make play_file() worse for files. What it shares: the PCM
ring, ring switching, the writer, sample-rate reconfiguration, volume,
ReplayGain's absence handled as unity, the sleep timer (it only writes
the output volume), and the portal's pause. Decisions it has to make:
  - *Watermarks.* Sound starts when the PCM ring holds 4 s; below 1 s it
    stops the writer and shows "Buffering", and resumes at 4 s. Estimates;
    the probe's 0.83x five-second windows are what they must ride.
  - *Pause.* A live stream cannot be paused and resumed where it was. Pause
    disconnects and drops what is buffered; play reconnects. Holding the
    connection open while paused would fill both rings and then stall the
    server anyway.
  - *Next and previous* move through the station list.
  - *The screen.* Station name (icy-name, else the list's name), the ICY
    title when it is not empty (WNZK's is " - "), no seek bar, a LIVE mark,
    and the netstream state when it is not playing. Non-Latin titles are
    ark12's coverage question, to be seen rather than assumed.
  - *Wi-Fi off, portal, sleep timer* all end or pause the stream through
    the paths that exist; none of them may leave a TLS session open.
  - *Internal RAM.* One TLS session at a time, ever: the probe goes when
    this lands, and nothing else opens HTTPS while a stream plays.

**Phase 4 -- choosing a station.** `stations.m3u` at the card's root,
`#EXTINF:-1,Name` then a URL per station, shown as a RADIO entry in the
chooser and played with a new BROWSER_PLAY_STREAM kind. It needs no
keyboard and can be edited anywhere. Searching radio-browser from the
setup portal's page on a phone comes after, reusing the portal's web
server and saving into the same file.

**Not in scope yet:** HLS (.m3u8), resolving .pls or .m3u playlists
served *by* a station, Ogg/Opus streams, recording, and any timeshift.

**Numbers the plan rests on** are all in the probe entries above: WNZK
is 512 kbit/s ADTS AAC-LC at 48 kHz stereo behind a 302 whose token
lives 60 s; first audio about 2.8 s after connecting; the link gives
1.05x idle and 0.97x beside card playback; one TLS session costs 13 KB
of internal RAM with mbedTLS in PSRAM; lwIP must keep its default window.

### Two things that cost a session each, so that they do not again

- **A hub is a real failure mode, exactly like the cable.** A USB drive
  that fails enumeration (`CHECK_SHORT_DEV_DESC FAILED`), or mounts and
  then wedges with `scsi_cmd_read10 failed` and ten seconds of transfer
  timeouts per settings save, was a hub delivering 4.64 V at 0.15 A.
  Direct connection: 4.95 V at 0.4 A, and every symptom vanished. None
  of those errors say "hub", which is why this is written down. Two
  sessions went into idle thresholds, current ceilings and expander
  races first.
- **"bus backend up" is not a connection**, and `eh_sdio`'s pin banner
  prints compile-time Kconfig defaults rather than the live
  configuration. Three builds were diagnosed off that line as having
  wrong pins when the pins were already right. `wifi.c` logs the struct
  the driver actually copies, and says which line to believe.

## The 0200 series: phases 3 and 4, and what the board said

**Read this first if you are picking up internet radio.** 0200-0207 took
the stream path from "netstream and netdec work, nothing plays them" to a
station chooser that plays. Every patch after 0201 exists because of a
board log, and **the board found six faults that reading did not** --
which is the argument for flashing early and often on this path rather
than writing it all and then testing.

### Where it actually is

- **Phase 1 and 2: unchanged and confirmed.** Three more runs put
  0121's backpressure beyond doubt: ring peak 100%, 41-46 s of
  cumulative full-ring wait, **0 bytes resynced** every time.
- **Phase 3: done and flashed.** `play_stream()` in player.c, beside
  `play_file()`. Sound on two stations, pause, resume, and a station
  that paces at exactly real time for three minutes with no rebuffer.
- **Phase 4: done and flashed, one bug outstanding** (below).
  `stations.m3u`, `stations.c`, a RADIO tab.
- **The probe is retired** (`STREAMPROBE_ENABLE 0`), as this file said
  it would be. Gated rather than deleted: it is still the only thing
  that characterises a new station.
- **The screen is not done.** See the open list.

### The two stations, and why both were needed

They bracket the problem and no single one of them would have sized the
buffers:

| | WNZK | WUOM |
| --- | --- | --- |
| format | 512 kbit/s AAC-LC, 48 kHz **stereo** | 64 kbit/s MP3, 44.1 kHz **mono** |
| pacing | **1.00x to the millisecond** over 57 s | bursts ~6.6x for 10 s, then exact |
| compressed ring | peaked 41%, never filled | pegged 100% for 50 s |
| internal free floor | **40216** (decoder open costs 14860) | 92183 |

**WNZK never offers a surplus.** The only way to build a PCM lead is to
hold the writer off, and once spent it cannot be earned back while
playing -- so a rebuffer costs the listener four seconds of silence,
permanently. On WUOM the same rebuffer is nearly free, because 256 KB at
64 kbit/s is about thirty seconds of audio. **Tune the watermarks
against WNZK and check they are comfortable on WUOM, never the other way
round**: WUOM makes every threshold look cheap.

AAC is the tight case for internal RAM and always will be; MP3 has about
16 KB more headroom because it pays no decoder-open cost.

### Six faults the board found, and the shape they share

Four of the six are the same mistake in different clothes: **a value
whose meaning depends on something not in the value.**

1. **`netstream_init()` was never called from app_main** (0202). The
   header had always said to; only the probe did, so the ring existed
   from 45 s into the boot and a stream asked for at 18 s was refused
   correctly. The refusal now names its cause, because "no ring" and
   "a session is already open" are the same silence from outside.
2. **IDLE means two things** (0203). `netstream_play()` posts a request
   and returns *without touching the state*, so the first reading after
   it is IDLE meaning "not started yet". Read as "stopped", it ended the
   stream on its opening step -- `99 ms silent, 0 frames`, with the whole
   connect appearing in the log afterwards because
   `netstream_stop_wait()` was waiting for a task still dialling. **The
   log read as a station hanging up and every line in it was the player
   hanging up on itself.** `seen_live` is the latch; the fix moved into
   streamplan.h to be tested.
3. **The amplifier was off for 161 seconds** (0204) while correct audio
   was written. ui_task idles the amp from one line in `player_loop()`,
   and the chooser branch above it ends in `continue`. For a file that
   never mattered -- choosing a file is what closes the chooser -- and
   **a stream is the first sound that can start with the chooser open.**
4. **Pause parked the decode loop forever** (0204). The writer stops, so
   the PCM ring fills, so the send blocks; its only exit was
   `s_pending_ready`, so the top of the loop -- where a pause becomes a
   disconnect -- was unreachable. The press logged nothing at all.
5. **A paused stream ended itself after exactly thirty seconds** (0205):
   `BUFPLAN_STALL_GIVEUP_MS`. Nothing in bufferplan was wrong; the
   mistake was asking it. The plan decides whether a stream *trying* to
   play can, and a ring emptied by the listener is not a stall. It also
   corrupted the two figures that matter -- a pause was adding a
   rebuffer and half a minute to `silent_ms`.
6. **`stations_load()` retried at 10 Hz for ever** (open, below).

**Two of those were an early `continue` skipping a postcondition below
it** (3 and 4, and the `!leaving` guard in 0205 is a third instance
caught before flashing). This file's long loops hide their
postconditions well; when adding a `continue`, read what is below it to
the end of the iteration.

### Things that are settled now and were guesses before

- **The PCM ring is always 16-bit stereo**; mono is widened before the
  send. So buffered-ms is `bytes / (rate * 4)` using the **output**
  rate, never the decoder's channel count -- computing it from
  `info.channels` would read half the true fill on WUOM and rebuffer
  against a full ring.
- **`play_stream()` must run on the main task.** 24576 is the only stack
  that clears `NETDEC_STACK_FLOOR`.
- **`storage_io_fread()` takes its own lease per chunk** and must not be
  called while holding one. Wrapping a read loop in
  `storage_io_acquire()` would hold the card across 64 KB and starve the
  decode loop -- the exact contention the BACKGROUND class exists to
  prevent.
- **Stop from inside the full-ring wait takes 99-199 ms**, against 39 ms
  from a quiet stream. `netstream_stop_wait(3000)` has ample margin.
- **A reconnect costs one resync** (125 bytes seen): the new body starts
  at its own frame boundary and the decoder hunts once.
- **radio-browser.info serves M3U directly** from its station endpoints,
  which is why `stations.m3u` is M3U and not a format of our own. The
  hand-edited path and the portal's future search converge on one parser
  with one test file.

### What is open

- ~~**`stations_load()` is called at 10 Hz when there is no station
  list.**~~ Closed by 0209, and worth keeping for the shape of it. The
  load retried until it returned true, and "no stations.m3u" is a
  **permanent** false -- so a card without one produced
  `no stations.m3u on any volume` every ~104 ms from boot, about five
  hundred lines in the first minute and two file opens a second on the
  card behind whatever was playing. It now latches on
  `storage_generation()` **whether the load worked or not**, because the
  attempt is what has happened and the result is not what decides
  whether to repeat it. Same error as 0804's `if (n <= 0) break`: a
  return value asked a question it does not answer. **Unflashed.**
- ~~**The screen shows only the station name.**~~ Closed by 0301, and it
  did not even show that -- see the 0300 series below. `s_stream_bottom`
  (the ICY title) and `s_stream_status` (Connecting / Buffering /
  Reconnecting / No signal) were computed every pass by streamplan.h and
  **read by nothing**; `s_streaming` was read only by the transport.
  There was no LIVE mark, and the seek bar was suppressed only by
  `s_can_seek`. The guess about why it was left -- that it needed `ui.c`
  and `ui_state_t` rather than player.c -- was right about the fix and
  wrong about the obstacle: see 0301.
- **No ICY title has ever been seen on WUOM** despite
  `icy-metaint: 16000`. WNZK's `" - "` proves the demuxer surfaces them,
  so suspect the plumbing rather than `icydemux` (4064 host checks)
  -- but it may simply be a station that sends empty titles.
- ~~**The chooser cannot reload the station list.**~~ Closed by 0304.
  `stations_load()` opens a file and ui_task must not block on the card,
  so a `stations.m3u` edited with the card in did not appear until the
  player was idle and reloaded. The obstacle was real and the answer was
  not to remove it: RLOD is a request, performed by the player task,
  answered by an epoch.
- **`stack low water 2756`** on the netstream task, stable across every
  run and the thinnest figure in any of these logs, on a task doing TLS.
  Not urgent; worth knowing.
- **The 45-second probe delay instruction was never followed** in any
  run, so the 0.97x-beside-card-playback figure and the internal-RAM
  contention with a file decoder open are still single-sourced from the
  original probe session. Load testing, deferred deliberately.
- **Next/previous never tested on hardware** -- every flash so far had
  no `stations.m3u`, so the list has never had two entries in it. Still
  open, and 0302 found part of the reason it stayed that way: with one
  station or none the next icon was lit by `playlist_has_next()`, so the
  button invited a press whose whole effect was one log line.

## The 0300 series: the screen, and three faults found by reading

**Read this first if you are picking up internet radio.** The 0200 series
ended with a station chooser that plays and a bar that does not say it is
playing a station. 0300-0302 are that bar. **None of the three was found
by a board log**, which is the opposite of the 0200 series and worth
saying: 0200's lesson was flash early, and it stands, but the faults it
could not reach were the ones that need a *second* thing to have happened
first -- a file played before a station, a list with two entries in it, a
reader four thousand lines from its writer.

### What was wrong, and the shape the three share

All three are **a value that is correct, published, and read by the wrong
thing or by nothing.** Not one of them is a computation that was wrong.

1. **The station name was never on screen** (0300). `play_stream()`
   retires the previous track's numbers and its visuals in an explicit
   list, and does not retire its text. ui_task does not read
   `s_display_name`; it reads `s_name_shown`, a copy taken only when
   `s_text_staged` is set, and it prefers `s_tags_shown.title` to that
   copy whenever the title is non-empty. Nothing in `play_stream()`
   staged anything, so the station name sat in `s_display_name` unread
   and the screen showed **the last file's title, artist and album for
   the whole broadcast**.

   Every phase-3 run missed it structurally rather than by luck: each
   chose a station from a boot that had played nothing, where
   `s_tags_shown` is zeroed and the fallback to `s_name_shown` is
   accidentally the right answer. The fault needs a file first, which is
   the ordinary way anyone reaches the RADIO tab.

   This also means the 0200 entry above understated itself. "The screen
   shows only the station name" was the best case, not the behaviour.

2. **`s_stream_top` and `s_stream_bottom` were declared below their
   reader** (0301), beside `play_stream()` and four thousand lines under
   ui_task. In one translation unit that is not a matter of taste: **the
   task that draws could not name them, so "computed every pass and read
   by nothing" was enforced by declaration order.** `s_streaming` and
   `s_stream_status` had been moved up for exactly this reason when they
   were added; these two were not, and they are the two with something to
   say. They are declared with `s_streaming` now.

   **Put a published value where its reader is, not where its writer
   is.** player.c is one file and the compiler is the only thing
   enforcing that rule; it enforced it silently for eight patches.

3. **The next icon asked the playlist during a stream** (0302). The press
   handlers have branched on `s_streaming` since 0206 -- next goes to
   `stations_next()` -- and nothing told the icon, which was still lit or
   greyed by `playlist_has_next(browser_order())`. Both answers it can
   give are wrong: lit with a one-station list, so the button invites a
   press whose entire effect is to log `no other station in the list`;
   greyed with a full station list if the folder behind the stream
   happened to be at its last track, which hides a button that works.

   **A handler that branches on a mode and an icon that does not is the
   same bug as an early `continue` skipping a postcondition** -- the 0200
   series found that one three times. Adding a mode to a press means
   finding everything that describes that press.

### What the bar does now

Three fields on `ui_state_t`: `live`, `stream_status`, `stream_title`.

- **`live` is its own flag and deliberately not `len_sec == 0`.** Those
  say opposite things. An unknown duration is a gap in what the player
  knows and the bar admits it -- bare groove, both clocks dashed, which
  is `stats_valid`'s whole reason for existing. A live stream is not
  missing a position: it does not have one and never will, and drawing
  the unknown-duration state for it **reports a fault where there is
  none**. Row 2 gets a filled LIVE pill with the status beside it on one
  baseline; row 3 goes blank rather than dashed, for the same reason.
- **The ICY title takes the album row**, keeping the order
  `streamplan_lines()` already chose: top is the station, bottom is the
  title. The station is what was chosen and what stays; the title changes
  underneath it, and the two swapping rows would read as the player
  having switched station. `C_ICON`, not `C_ALBUM` -- three greys exist
  to rank three rows and there are two rows here.
- **The status crosses as a string, through
  `streamplan_status_text()`.** It is a function of netstream's state
  *and* the buffer phase, those two disagree on purpose, and streamplan.h
  holds the table and the host test for reconciling them. Handing ui.c
  the enum would put a fifth copy of that reasoning in a draw call.
- **Both gates in `ui_draw()` are one-line extensions of existing
  `if`s**, so the 130 lines of seek and clock logic are untouched and
  unreindented. Nothing new is a hit target: `ui_touch()` already gates
  the seek drag on `can_seek`, which `play_stream()` holds false for the
  life of the stream.

### What was checked, and what that is worth

`ui.c` compiles clean at `-O2 -Werror` against the texttest fakes, which
catches the layout arithmetic and the format strings and **nothing about
how any of it looks**. `ui.c` is not in the texttest binary and cannot
easily be: it needs `esp_lcd_panel_ops.h` and, through `audio_out.h`,
`driver/i2c_master.h`. Adding those fakes would buy the geometry a test;
it is not obviously worth a fake I2C bus.

**Unflashed.** 0300 is testable in one step and the step is worth writing
down because no run has done it: **play a tagged file, then open RADIO
and pick a station, and read the three text rows.**

### What is open

- **It builds and boots, and none of it has been exercised.** 0300-0303
  went on the board as `v0.3.0-138-gc2dd970` and the boot is clean and
  identical in shape to the one before it: no new warnings, the file path
  unchanged, `no stations.m3u on any volume` still once and only once.
  Then the RADIO tab was tapped at 560 s and said `radio: 0 stations`,
  which is what it has said on every run of this series.

  **Nothing in the 0300 series has drawn a pixel.** No LIVE badge, no
  station name, no status line, no ICY title, no lit next icon -- all of
  it needs a stream, and a stream needs a station list. Two flashes have
  now confirmed the patches compile and confirmed nothing else about
  them, which is the trap the 0200 series' six board-found faults warn
  about, arrived at from the other side: flashing early does not help if
  the path cannot be reached.

  **0304 is the unblock and is the thing to flash next.** Write a
  `stations.m3u` (`stations.m3u.example` is in the tree), tap RADIO, tap
  RLOD. Then the 0300 test that has never been run: play a tagged file
  first, and only then pick a station.
- **"No signal" looks like "Buffering".** All four statuses draw in the
  same white. One is terminal and three are not, the difference is worth
  drawing, and it cannot be recovered from a string by comparing prose in
  a draw call. It needs its own flag from the player.
- **No elapsed counter.** How long a station has been playing is a real
  number and arguably owns the left-hand clock slot. Nothing counts it,
  so it needs a counter in `play_stream()` and a decision about whether a
  reconnect resets it. Left rather than guessed.
- **The marquee stays on the station row**, though the ICY title is the
  line most likely to overrun 19 characters. There is one marquee and the
  title row owns it; handing it to whichever line is longest is a change
  to how the marquee is owned, not a change to what is drawn.
- **The first join after a scan fails, reproducibly, and costs 10 s.**
  Not new and not this series -- `wifi_join()`'s comment block already
  records `boot, first attempt -> reason 2 after 6.6 s` and the six
  flashes that established the whole-attempt retry. What these two runs
  add is that it is **reproducible to about twenty milliseconds**: scan
  ends, `set_config`, `WifiEventNoArgs id=43` about 2.45 s later, then
  reason 2 at 6.64 s, then the retry joins ~4 s after that. Both logs.
  Address at 18.2 s and 18.7 s from boot against about 8 s if the first
  attempt worked.

  The one thing in the timing that is not already written down: in both
  runs the failing attempt was issued **within 13 ms of a 30-network
  scan finishing**. That is a hypothesis and nothing more -- the sixth
  flash had a reason-2 with no scan and no 43 before it -- and per the
  cyan flash it is worth instrumenting before it is worth patching. The
  retry works and the cost is ten seconds of a boot that is not playing
  yet.
- **The compressed ring's fill is still drawn by nothing.**
  `s_ring_pct` is fed from `netstream_ring_pct()` every pass and row 2
  now has a 72 px band with a pill in it and room to spare. It is
  telemetry rather than something a listener asked for, which is why it
  was not added here.
- ~~**The chooser still cannot reload the station list.**~~ Closed by
  0304. **`stack low water 2756`** on the netstream task still stands,
  carried over from the 0200 list unchanged.

## The level strip (0329-0333), and two ways of being wrong

### What it is

A minute of output level behind a mark, the buffered reserve ahead of
it, in the envelope's own two colours -- `C_WAVE_PAST` behind,
`C_WAVE_FUTURE` ahead, `C_PLAYHEAD` for now. A stream's bar says the
same thing a file's envelope says; only the units differ, seconds of
buffer instead of minutes of file. The grey is bufplan's `buffered_ms`,
the same figure netstream's line prints as `audio Xs`, so a photograph
and a log are the same quantity by construction.

Built in the order the plan headers were: `levelhist.h` with 42 host
checks and no data, then the writer filling it, then `ui.c` drawing it.

**The grey is the PCM ring, not the encoded ring.** That question came up
and is worth answering here: `stream_buffered_ms()` reads
`xStreamBufferBytesAvailable(s_pcm)` over `rate * 4`. The encoded ring is
the `bytes 0% (2048)` on the same line, and it is *meant* to sit near
zero -- netdec drains it as fast as bytes arrive.

### THE BUG NO TEST COULD HAVE CAUGHT

`levelhist_read()` returns the whole ring, oldest first, so the newest
column is at index 239. The mark is at 160. 0332 drew columns 0-159 as
the history, which is **the oldest forty seconds**, putting the most
recent twenty off the end of the strip behind the reserve.

From cold that means nothing red is drawn for the first twenty seconds,
while the grey grows normally throughout -- so the two looked like they
were taking turns. It compiled, every existing test passed, and it was
invisible to anything except a person watching the panel for a minute.

**This is the class of fault the whole project is weakest against.** The
0300 series' table is about values that answer the wrong question; this
is a value that answers the right question and is *read from the wrong
end*. Nothing in a compiler or a host test can see it. The only
instrument is eyes on the panel, which is also what found the three
faults in 0315-0316 after fifteen patches of clean logs.

### AND TWO PROCESS FAILURES WORTH MORE THAN THE PATCHES

**0330 did not build, and broke the rule this file states.**
`s_stream_buffered_ms` was declared beside `s_stream_audible`, four
thousand lines below its only reader. That is the exact fault 0301 fixed
and that the table above tabulates under a heading saying *put a
published value where its READER is*. **Writing a rule in CLAUDE.md does
not enforce it.** What would have enforced it is a script, and the
declaration-order audit that eventually caught the rest should have been
running since 0301 rather than after the second failure of the same kind.

**And 0330 shipped with an edit that silently never applied.** A script
threw partway through and lost four changes without writing the file;
the symbols were checked for existence rather than for *use*, so
`levelhist_note_silence()` was defined and never called. The compiler
said so, as a warning, and the warning was read as noise from an edit
that had not landed yet rather than as the edit that had not landed.

That one is the worse of the two and the reason it is written here: an
unbuildable patch stops. That one would have built, run, and drawn an
unbroken minute across every dropout the strip exists to report -- a
display lying in exactly the case it was written for, with nothing in
the log to contradict it.

### Open

- **The reserve is a flat block.** Deliberate -- nothing is known about
  what the buffered audio sounds like, only that it exists -- and it
  reads as featureless. Shaping it needs a carrier of per-block peaks,
  because FreeRTOS stream buffers cannot be peeked and reaching into the
  storage behind a blocked reader is 0507's fault. See `streamgain.h`,
  which wants the same carrier and should own it.
- **`LEVELHIST_NOW_COLUMN`, the band height fractions and the three-pixel
  clip marker are all guesses** a photograph would settle in seconds. The
  ring deliberately holds a full minute while only forty seconds is
  drawn, so moving the mark is a one-line change rather than a resize.
- **`stalled` climbs steadily while the reserve holds flat.** SomaFM
  reached 1313 ms over two minutes with `audio` rock-steady at 17.6s.
  That is the writer being held off, growing on a station that is not
  short of audio, and nothing explains it yet.

## streamgain.h (0334): a draft, and a name that was wrong

Slow loudness levelling for a stream, written down before the decode
loop is touched. Nothing calls it; two functions are declared
unimplemented because the gating belongs lifted out of `loudness.c`
rather than copied.

**It is not ReplayGain and the first draft's title said it was.** The
honest description: a gated mean over the blocks in a twenty-second
window -- essentially a twenty-second average with BS.1770's weighting
on top. Over a five-minute track the relative gate discards the quiet
intro so the loud body sets the number, which is why ReplayGain is not
an average; over twenty seconds the window can be *entirely* quiet
intro, with nothing louder to gate against, so it degrades toward a
windowed mean. The gain moves within a track. That is compression with a
twenty-second time constant.

**And there is no way around it.** Per-track gain needs the whole track
before the first sample plays, and twenty seconds of lookahead gives
that only for tracks under twenty seconds. The ICY title marks a
boundary on SomaFM and never on WUOM or WNZK, and even then the
measurement finishes after the audio it describes has been heard. The
honest comparison is broadcast R128 with a slow follower.

Which makes `STREAMGAIN_SLEW_DB_S` the entire design, and it has no
measurement behind it. Of the three stations ever tested, only SomaFM
has the dynamics to say whether a value is right -- WUOM is talk and
WNZK has been `" - "` for its whole recorded history.

## A VALUE THAT ANSWERS A DIFFERENT QUESTION (read this one)

**Five faults in this series were the same fault.** Not the same code,
the same shape: a value is read to answer a question it does not answer,
the reading is plausible, and the wrong answer looks like someone else's
bug. They are listed together because the fifth one was found by
recognising the pattern rather than by instrumenting, and that is the
first time on this project that reading this file was faster than
reflashing.

| value | asked | actually means |
| --- | --- | --- |
| `NETSTREAM_IDLE` (0203) | is the source done? | done, **or not started yet** |
| `stations_load()` returns false (0209) | should I retry? | no file, **which is permanent** |
| `STREAM_CODEC_NONE` (0309) | keep sniffing? | unidentified, **or identified as not audio** |
| `uxTaskGetStackHighWaterMark()` (0310) | how much stack is left? | **the least there has ever been** |
| `phase_since_ms` vs 30 s (0311) | has the source given up? | **how long the phase has lasted** |

Two things they share, and both are the tell:

**The wrong answer accuses something else.** 0203's log read as a station
hanging up; 0311's read as a station hanging up; 0310 blamed the caller's
stack size in a message naming the correct size; 0309 blamed the station,
which for once was fair, 1300 times. **If a log accuses a thing you
cannot see, suspect the value that named it.**

**The evidence of the bug is usually in the number itself.** A task whose
worst-ever free stack is 2068 has already survived the 17744-byte call it
is being refused for. A stream that ended at `30025 ms silent` against a
30000 ms limit ended 25 ms after the attempt that succeeded. Read the
figure before believing the sentence built from it.

There is a third of these in CLAUDE.md's own opening: a stack protection
fault reports the overshoot, not the demand. That one cost two patches
and two panics. This class is the most expensive thing on this project.

## The 0308-0311 flashes: radio works, and four faults on the way

Four runs, each one finding something the previous had hidden. Worth
reading as a sequence, because the order was not optional -- **0308's
fault was masking 0310's, and 0310's was masking 0311's.**

### What works now

- **All four stations play.** MP3 at 64 (WUOM) and 128 kbit/s (SomaFM),
  AAC-LC at 48 kHz stereo (WNZK). Pause and resume on a stream. Station
  to station without going through the file path.
- **ICY titles, at last**, and the answer to the question 0200 left
  open: `"Zero Cult - City Voices"` and `"D. Batistatos - For All I
  Know"` from SomaFM, 1.4 s after connect. **The demuxer and the
  plumbing were always fine.** WUOM sends `icy-metaint: 16000` and no
  titles, and WNZK sends `" - "` forever, which `streamplan_lines()`
  suppresses correctly. Two stations that send nothing is not a bug.
- **0200's WUOM measurement reproduced exactly**: ring climbs to 69% on
  the front-loaded burst, then holds while the rate settles to 62
  kbit/s. WNZK still sits at 0% for its whole life, as predicted.
- ~~**SomaFM is the third pacing case** and sits between the two: 128
  kbit/s, ring 0-2%, 300 ms of cumulative stall in half a minute.~~
  **Wrong, and wrong from one run.** Corrected below.

### The four faults

1. **`browser_open()` never reset `s_radio`** (0308). Once the RADIO tab
   had been visited, every REOPEN of the chooser drew a volume's files
   with the radio flag still set -- `button: row 3 (station)
   "Advent_Chamber_Orchestra_-_04_-_Mozart..."` followed by
   `station 4 of 4: ice1.somafm.com`. Broken since 0207 and unreachable
   until a station list existed, a station had played, AND the chooser
   was reopened. **It also made every other fault in that run
   unreadable**, because half the presses were being dispatched as the
   wrong kind of thing.
2. **A web page was a delay rather than a verdict** (0309). 1300 lines of
   `The station returned a web page` in thirteen seconds, and the stream
   never ended. See the table above.
3. **The stack check read the high-water mark** (0310). Every stream
   after a file had played was refused for the rest of the boot. The
   changing figure was the clue -- 5604 after a stream, 2068 after a
   256 kbps file -- and **no instrumentation was needed**: a value that
   moves when nothing relevant has changed is not measuring the thing.
4. **The stall giveup was wall clock** (0311). This file had claimed a
   source working through retries could never be cut off, reasoning from
   a 15 s backoff against a 30 s limit. **The arithmetic left out how
   long an attempt takes**: six seconds each against a server that
   handshakes and then times out, five of them, and the stream died
   25 ms after the fifth succeeded.

### Two things the logs settled that were guesses

- **zeno.fm is the flaky one, not the player.** Three runs show
  `mbedtls_ssl_handshake returned -0x0050` on the first attempt to
  `stream.zeno.fm`, recovered by the whole-attempt retry, then a 302 to
  `stream-285.surfernetwork.com`. It is worth keeping in the station
  list precisely because it is unreliable: it is the only station here
  that exercises the retry path, and it is what found 0311.
- **The AAC decoder-open cost is not a constant.** 14860, 14864, 14872,
  10888, 10896 across runs, all on the same station. `netdec_reconnect()
  does close and reopen, so it is fragmentation rather than a leak --
  but the internal-RAM budget was tuned against 14860 as though it were
  fixed, and the spread is 4 KB.

### SomaFM front-loads, and the 0% run was the link (0314)

The clean run above measured the same station at **ring 12% -> 32% ->
46% -> 70% -> 89%, then holding at 89-90%** while the rate settled to
128 kbit/s, with 30 ms of cumulative stall in forty seconds and no
rebuffers. That is WUOM's shape, not a third one: burst, fill, hold.

The run that said 0-2% and 300 ms of stall was the same station, the
same URL, and the same firmware. **It was the link.** That run also
reached SomaFM through `first sound at 4040 ms` against 537 ms here, and
one of its windows reported 124 kbit/s on a 128 kbit/s stream -- a
station delivering audio slower than real time, which is the definition
of not being able to build a lead.

**Two stations bracket this problem and a third does not exist.** WUOM
and WNZK were picked in 0200 because one always has a surplus and the
other never can; a station that front-loads on a good connection and
starves on a bad one is not a new case, it is the first case in bad
conditions. Tune against WNZK, as 0200 said.

**What this cost was one patch, and it is the cheapest instance of the
mistake in this file.** A pacing figure was taken from a single session
and written up as a property of the station -- which is the same error as
the 0.97x-beside-card-playback number that is still single-sourced and
still flagged in the 0100 list, and the same one 0116 caught by measuring
four times. **A throughput measured once is a measurement of that
afternoon.** Anything in these notes describing how a station behaves
needs two runs before it goes in, and this entry exists because this one
did not have them.

### What is open

- **The screen has still never been looked at.** Every fault in these
  four runs was found in the log, and 0301's LIVE badge, status line and
  ICY title row have been on a working stream for minutes at a time with
  nobody reporting what they looked like. That is now the cheapest
  unknown in the project.
- **`first sound at 11986 ms` on a resume**, against 482 ms on a
  reconnect to a fast server. Most of it is the connect, but about five
  seconds is preroll against a station whose ring never fills. Whether
  BUFPLAN_PREROLL_MS is right for a 0%-ring station is a real question
  and no number in these logs answers it.
- **`stream cannot be decoded; giving up` prints before
  `buffering -> playing`**, which reads as the give-up being ignored. It
  is not -- two tasks, two log lines, no ordering between them -- but it
  is the kind of line that costs somebody twenty minutes.
- **`only 2048 audio bytes before the drop` after a give-up.** True and
  misleading: the drop was ours. Same family as 0205.
- **The 0311 case has not recurred.** The run that confirmed the patch
  had a single retry, not five, so the fix is flashed and the fault it
  fixes has not been re-triggered. Held open deliberately.

## WNZK starves on the device and not on a PC (measured, 0328)

**The device is the limit, not the station.** Same three stations, same
minute, 300 s each, measured with `tools/streamcheck.py` on a PC on the
same network:

| station | declared | PC mean | PC ratio | device mean | device ratio |
| --- | --- | --- | --- | --- | --- |
| WUOM | 64 | 74 | 1.15x | 62-72 | ~1.0x |
| SomaFM | 128 | 135 | 1.05x | ~220 filling, then ~128 | ~1.0x |
| WNZK | 512 (0200) | **522** | **1.02x** | **466** | **0.91x** |

Zero simulated dropouts on the PC for all three, and a reserve that sits
flat at 50 s, 21 s and (WNZK) never falls. The device sawtooths 4.1s ->
1.4s on WNZK with a dropout every 35 s.

### What this does and does not prove

It rules out the station and the path to it. It does not prove a hard
ceiling, and the honest reading is narrower than "the transport is
capped": **the device's own windows ranged 392-552 kbit/s and its best
window beat the PC's 300-second mean.** So the device is not blocked
below 512; it averages just under while the PC averages just over. An
11% shortfall on the one station in the list heavy enough to need the
full rate.

That is consistent with the `CP without SDIO SW_AGGR; compatible
streaming mode enabled` hypothesis and does not confirm it. Aggregation
would plausibly close 11%. So would several other things. **What is
settled is only that no amount of buffering on the device fixes this,
and that spoofing a user agent or a client identity would change
nothing** -- the PC sends a different User-Agent and gets the same bytes.

### Two corrections to earlier entries

- **0322's icy-br fallback does not help WNZK.** The whole reason it was
  written was that the AAC path reports no bitrate and WNZK's card had
  no kbps line. It turns out **WNZK sends no `icy-br` either** -- not to
  the PC, and not to the device, where the header dump has always shown
  only `content-type`, `icy-name` and `icy-metaint`. The patch is still
  right for stations that do send it; it simply does not fix the case
  that motivated it. The card will still show `AAC` and `48000 Hz
  stereo` and no rate for WNZK.
- **The 512 kbit/s figure for WNZK is 0200's, not the station's.** With
  no `icy-br` there is nothing to read it from, so every ratio in the
  table above depends on a measurement taken months earlier. The PC's
  522 kbit/s mean is consistent with it and is not independent
  confirmation.

### Do not act on the version mismatch without more than this

`main/idf_component.yml` says, and said before any of this:

>  DO NOT act on it without a reason: an OTA would replace M5's C6
>  firmware with a stock slave build, which has never been tried on this
>  board, and this tree has nothing that writes one.

An 11% shortfall on one station is a reason to investigate and not yet a
reason to reflash a coprocessor that cannot be recovered without hardware
this project does not have. **Downgrading the host to 2.12.x to match is
not the alternative it looks like:** SW_AGGR is a 3.x feature, so
matching downward keeps the compatibility path and loses the Tab5 board
preset that sets the reset line through Kconfig -- the one pin wifi.c
cannot set from code.

## radio-browser.info, which had never been touched (0306)

**The question that prompted this: wasn't radio-browser integration
supposed to happen?** It was, it is the headline of v0.4.0, and before
0306 not one line of code mentioned it. Worth recording plainly, because
the reason is instructive rather than an oversight: everything built so
far was *scaffolding for* radio-browser -- `stations.m3u` is M3U because
radio-browser serves M3U, `stationlist.h` says so in its opening
paragraph, `netstream` does the TLS, the portal does the typing -- and
scaffolding that is described in terms of a thing is easy to mistake for
the thing.

### The bet held, and it had never been tested

`stations.h` and `stationlist.h` both claimed the server's M3U goes
straight into the existing parser. **It does, and by rule rather than by
luck.** The real response is not what `stations.m3u.example` shows:

    #EXTM3U
    #RADIOBROWSERUUID:01234567-89ab-cdef-0123-456789abcdef
    #EXTINF:-1,Best Radio
    http://stream.example.com/mp3_128

The clause in `stationlist_parse()` that skips every `#` line which is
not `#EXTINF` was written for files that have been through another
player, and this is that case. `radiobrowsertest.c` carries the
documented response as a fixture and it parses to four stations with the
right names. **Had it failed, two headers and the choice of M3U over a
format of our own would have failed with it**, and it would have failed
on the board, three patches into a fetch.

### Four API facts, each of which would otherwise cost a flash

- **The pool name cannot be used over TLS.**
  `all.api.radio-browser.info` is the documented way to find a mirror and
  is a DNS construct rather than a server, so no certificate is valid for
  that name. This player verifies certificates. Hence a short hardcoded
  list of named mirrors -- the exact thing the DNS lookup exists to avoid
  -- accepted with the exit written down: the fix is the SRV record
  `_api._tcp.radio-browser.info`, esp_netif has no SRV resolver, and
  adding one is a larger piece of work than the feature.
- **`/m3u/stations/search?name=` and not `/m3u/stations/byname/{term}`.**
  Both exist. In the second the term is a path segment, and an encoded
  slash in a path segment is rewritten or rejected by enough
  intermediaries that it is not worth finding out which.
- **The service asks for three things**, and they are the price of
  having no key and no account: a descriptive `appname/appversion`
  User-Agent, no more than 2-3 requests a second with results cached
  5-15 minutes, and `/json/url/{stationuuid}` called when a listener
  starts a stream so the directory can count what is actually played.
- **The default `limit` is 100000**, which is a 40 MB M3U into a 64 KB
  buffer.

### The thing that will be got wrong if it is not written here

**A `stations.m3u` written from a search must copy the response body
through verbatim.** Not be regenerated from parsed `station_t`s, which is
the obvious way to write it and which silently discards
`#RADIOBROWSERUUID`. The click count is the only obligation this service
imposes, it needs the UUID, and the UUID exists nowhere but the response.
Rebuilding the file loses the ability to ever honour it, and loses it
invisibly -- the stations still play.

### What is open, in the order it unblocks

- **Nothing fetches anything.** 0306 is the request, not the call. The
  next step is the GET on the netstream task or a worker, the mirror
  retry, and the write through a temp file and a rename the way
  `settings.c` does -- `stations.h` already says the write goes that way
  and why.
- **There is no way to type a search term.** There is no keyboard and
  there will not be one; the portal is the answer and is why
  `portalweb.c` exists. That makes the search a form on a phone, which
  means the portal has to be reachable while the player is on a network
  rather than only while it is an AP -- and that is a change to
  `portal.c`, not to this.
- **`RADIOBROWSER_HOSTS` will rot.** Two names, and the docs say either
  may go away. Nothing monitors this and nothing can; it costs a failed
  search and the second name.

## Where v0.3.0 got to (the 1000 series)

**Read this first if you are picking this up cold.** The 1000 series was
one session, aimed at clearing the open list before tagging v0.3.0. It
found three real bugs and a great deal of documentation that had stopped
being true, and the proportion is the point: **most of what looked open
was already closed, and most of what was actually broken was not on any
list.**

Three bugs, all found by a file rather than by reading:

| | |
| --- | --- |
| 1005 | a 64-bit divide per pixel in the PNG scaler tripped the task watchdog on a 1600x1600 cover. RV32 has no 64-bit divide |
| 1009 | a FLAC with 1.8 MB of metadata never reached its audio: the ES parser gives up at 512000 bytes. A whole album played nothing |
| 1000 | `duration.c` filed the playing track's reads as PREFETCH |

Everything else was the list disagreeing with the code. 1002 read every
open entry against the source and found four stale and one *corrupted* --
a bullet that asserted a restriction and denied it four lines later. 1008
found the PCM ring described as 64 KB, 256 KB and 10 MB in six places
when it is 3520 KB, and the heap-corruption section still saying the ring
stays small because of an allocator the code no longer uses.

**The method that worked, and it is the file's own:** measure before
patching. 1003 was asked for a peak limiter and logged the peak instead,
which is how 1004 could prove the format claim false and 1006 could close
`peaking?` for a reason nobody had guessed -- real music does reach the
rail, on the unity pass, and still never clips because the gain that
follows is -11.96 dB. 1010 was asked whether to reclaim the cover cache
and measured instead, which killed the idea in two log lines: the only
cache tenant at the moment of failure was the compressed image being
decoded.

Twice the measurement corrected the patch that had just been written.
1001 had to fix 1000's own falsification condition, which named a window
the probe cannot run in. That is worth keeping as the shape of the
session: **a check written from the code rather than from where the code
runs is not a check.**

### What is confirmed on hardware

1000 (the probe's reads absent from PREFETCH's per-track total), 1005,
1006, 1009 and 1011 all have board logs behind them. 1003's
instrumentation has run on five formats and two libraries and
`clamp_hits` has never once appeared, which is the prediction that would
mean the ReplayGain headroom cap is wrong.

### What is open, and what each one needs

- **A 3000x3000 cover cannot be displayed.** Still true, and now for a
  reason that has been narrowed twice. The hardware wants 17672 KB in one
  block against a largest free block of 16128 KB. **That 16128 figure is
  structural, not fragmentation**: two board logs 223 seconds apart,
  across a seek that dropped 3.5 MB of queued audio, a track change and a
  cache that went from one entry to three, report it bit-identically.
  Churn does not reproduce to the kilobyte. 18036 - 16128 = 1908 KB of
  free PSRAM is stranded behind something permanent, and `gfx.c`'s
  1800 KB shadow buffer is the obvious candidate -- **unconfirmed**, the
  108 KB difference is unaccounted for, and a
  `heap_caps_print_heap_info(MALLOC_CAP_SPIRAM)` at boot would settle it.
  If it holds, 1010's "365 KB short of everything" measured against a
  total that is unreachable by construction, and the hardware path was
  never going to decode this cover on any build with a framebuffer.
  Verified true (1101): the hardware decoder has no scale field. The
  TJpgDec fallback is therefore the only path, and 1104 fixed the reason
  it could not start. **Whether it now produces a picture is unflashed.**
- ~~**The cover cache holds whole compressed images** -- three copies of
  the same 1.8 MB picture on an album that shares one.~~ **Half closed by
  1102**, and it is worth being precise about which half. The three
  copies are gone: identical covers now share one refcounted buffer, so
  an album costs one picture rather than three. The cache still holds
  *whole compressed images* and a 4 MB cover is still 4 MB. The three
  behaviour changes 1006 declined to choose between -- cap, downscale,
  do not prefetch past a size -- are all still open and still not
  equivalent. 1102 was the option nobody had listed, because it is the
  only one that is not a behaviour change. **Not flashed.**
- **The decode skip** (1011b), which needs a retained decoded cover and
  a hash on the prefetch path. See 1011.
- **Gapless**, unstarted and no longer blocked.
- The one storage lease, AMR's missing test file, UAC 2.0 and bus power,
  all unchanged and all still correct as written.

### The thing to keep doing

Every stale entry in this file was fluent and plausible, and the one that
contradicted itself in plain sight survived just as long as the rest --
which says the list was not being read rather than being read and
believed. **Read the open list against the code before trusting it**, and
prefer a log line to a feature when the question is whether the feature
is needed.

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

## The 0500 series: Ogg, and two numbers that were measured in the wrong place

0500 was asked for by a log in which a station connected, delivered, and
gave up three seconds later on `Ogg streams are not supported yet`. What
came out of it was one feature and two measurement faults, and the two
faults are the part worth keeping.

### The feature was one line of routing and a constant

**The refusal was never a statement about capability.** `decoder.c` has
played `.ogg` and `.opus` files through
`ESP_AUDIO_SIMPLE_DEC_TYPE_OGG` since the beginning, and `netdec.c`
already drove that same component for ADTS AAC. Nothing was missing
except the wire between them.

What makes it work is that **`_OGG` is the container parser, not a
codec.** netdec.h's list of decoders a sliding window cannot reach names
`_RAW_OPUS` and `_VORBIS` and both belong on it -- they want exactly one
encoded frame per call. `_OGG` takes arbitrary lengths, finds its own
page boundaries, and decides for itself what is inside. That is the same
property `_AAC` with `use_frame_dec = false` has, and the same argument
`decoder.c` already makes for files. Broadcast Opus is Ogg-encapsulated
without exception, so the restriction that rules out the bare codec
never reaches a station.

So the AAC half of netdec.c was generalised rather than copied:
`esp_dec_open()` and `esp_read()` serve both, differing by one
`dec_type` ternary. Most of that diff is a rename. The alternative was
two functions identical in their window handling, their
consume-before-error ordering and their format checks -- four places to
fix a fault twice.

Measured on the board, first run:

    first frame: Ogg, 48000 Hz, 2 ch, 16-bit, 704 bytes -> 960 samples
    Ogg decoder open (parser-framed); internal free 107607 -> 107783

**The Opus decoder costs nothing measurable in internal RAM.** Two runs
reported +176 and -176 bytes, which is noise on a heap that moves by
kilobytes in the same second. AAC cost 15.8 KB and 0115 exists because
that pool is what the Wi-Fi transport needs; this one is not competing
for it. The figure was unknown when 0500 was written and the open's
free-before/free-after line is what answered it -- which is the only
reason that line exists.

### THE WINDOW IS SIZED BY THE FORMAT WITH THE WIDEST FRAME, AND OGG IS NOW IT

framewin.h's rule has not changed: a window smaller than the largest
frame means a frame that never fits, a decoder that consumes nothing,
and a **hang rather than a glitch**. What changed is the number. ADTS
carries a 13-bit length field and stops at 8191 bytes; **an Ogg page
runs to 65307** -- 27 of header, 255 segment lengths, 255 * 255 of
payload. 24 KB could not hold one.

160 KB: two maximum pages and half of a third, in PSRAM where the extra
136 KB is nothing. A real Opus page from a radio station is a couple of
KB and none of this is visible on the board. The constant is for the
station that pages coarsely, and that station is not rare enough to meet
with a spin.

### AND WIDENING IT BROKE A FIGURE THAT HAD BEEN CORRECT BY ACCIDENT

This is the entry to read twice, because it is **the third instance of
the same fault in this file**.

The delivery line's denominator comes from `cost_report()`: compressed
bytes per second of decoded audio, which is what a station costs. On the
first Ogg run it read like this, on one station, one encoder,
`icy-br: 192`, with a 16 second reserve and no drops:

    163 kbit/s of 524 needed  (31%) SHORT
    221 kbit/s of  16 needed (1381%)
    205 kbit/s of 606 needed  (34%)

**It was counting bytes into the WINDOW, not bytes into the decoder.**
`s_cost_bytes += got` lived in `refill()`. Bytes sit in the window until
a frame is complete, so each reading divided one second of samples by
whatever happened to arrive while they were produced -- and those two
points are separated by however much audio the window holds. At 24 KB
that was about a second at broadcast rates, the error stayed inside a
reading, and 0416 confirmed the figure on two stations. **0500 made the
window 6.6 seconds wide and multiplied a latent fault by six and a
half.** `of 16 needed` is the window full; `of 606 needed` is the window
refilling. Neither is the station.

The fix is to take the delta of `framewin_t`'s own `out` -- the bytes
the decoder consumed, which are exactly the bytes that became these
samples. Correct at any window size, which the old one never was.
Confirmed on the next run: 202-205 on every line against a declared 192,
delivery 189-225 around it, SHORT on the preroll line only.

**THE RULE, WHICH HAS NOW COST THREE PATCHES.** A ratio whose two halves
are measured at different points is not a measurement:

- 0414 divided delivery by consumption where both were the compressed
  ring, and got 100% on every line. A tautology.
- 0502 divided samples by arrivals where only one end was the decoder,
  and got a number that followed the buffer instead of the station.
- The general shape is already written down as A VALUE THAT ANSWERS A
  DIFFERENT QUESTION, above, and neither of these was caught by reading
  it.

When a figure changes because a BUFFER changed size, the figure was
never about the thing it names. The board is what found both of these,
and in both cases the wrongness was visible as an implausible SPREAD --
a factor of 38 here -- rather than as an obviously wrong value.

### A stop is not a drop (0501)

The same first log ended with `only 12288 audio bytes before the drop`
after the decoder had given up and `play_stream()` had torn the stream
down. `pump()` returns on a stop request as much as on a dead socket.
The branch beside it already withheld its line during a teardown, for
exactly this reason and with a comment saying so; the failure branch
never got the same treatment, and incremented `s_failures` as well.
Found only because 0500's fault caused a stop early enough to fall under
`netplan_made_progress()`'s threshold -- a station stopped by the
listener after a minute takes the other branch and never showed it.

### Progressive JPEG: what was tried, and what is actually left

WALM's `icy-logo` is a progressive JPEG and neither decoder here reads
one. **This is settled, not open**, and the reasons are worth recording
so nobody re-derives them:

- The P4's engine handles SOF0 only. TJpgDec handles SOF0 only: in
  `tjpgd.c`, SOF1 through SOF15 fall to `return JDR_FMT3` inside
  `jd_prepare()`, before a scan byte is read. There is no configuration
  that changes it. `decode_software()` is a fallback for MEMORY, not for
  FORMAT -- it exists for the cover the hardware cannot get a contiguous
  block for, and a progressive file fails it identically.
- **TJpgDec's 1/8 path is DC-only**, and the first scan of a progressive
  file is DC-only, so decoding just that first scan is a real
  possibility -- roughly fifty lines against a vendored fork of
  `tjpgd.c`. It yields width/8: a 150x150 logo becomes 18x18, which is a
  smear. It would be worth having for a 3000px cover and is worthless
  for a station logo, which is the size stations actually use.
- A progressive decoder cannot be low-memory. Progressive is defined as
  coefficients spread across scans, so the whole coefficient array must
  exist before any IDCT: **3 x W x H bytes for 4:2:0**, MCU-padded. 77 KB
  for this logo, 3 MB for a 1000px cover, 27 MB for a 3000px one. PSRAM
  makes the first two free and the third impossible.
- The candidate, if this is ever wanted: **`stb_image.h`**, which states
  baseline and progressive support with the stock IJG exclusions, is one
  public-domain C header, and has `STBI_ONLY_JPEG`, `STBI_NO_STDIO` and
  `STBI_MALLOC`. Same vendoring pattern as minimp3 and pngle. It cannot
  downscale, so it needs a pixel ceiling above which the current refusal
  stands.

0503 took the cheap route first: ask the directory for a different
picture when the logo will not decode. `radiobrowser_favicon()` had
existed since 0311 and was reached only when `icy-logo` was ABSENT --
absent and unusable are not the same thing. **The board answered it in
one run: WALM's favicon is byte-for-byte the same URL as its logo**, so
the `strcmp` guard fired and the second fetch was not spent. The station
shows the text card, and the patch is still right for the stations whose
two URLs differ.

Two things 0503 got slightly wrong, for whoever is next:

- When there is no alternative URL, the undecodable bytes are still
  handed to the draw path, which calls `cover_drop()` and walks the
  markers again before logging the same refusal. Freeing them in that
  branch is one line.
- The check is asked in `do_stream_art()`, on media_task, with a marker
  walk rather than a trial decode. That placement is the correct half
  and should not be moved: the draw path is a blit on a repaint and has
  nowhere to go, which is why the failure was useless where it was found
  originally.

## The 0900 series: why lossless will not play, and four things it was not

The question was why FLAC streams would not play when MP3 and AAC ones
would. The answer turned out to be arithmetic, but it took four
eliminations to get there and each one cost a theory.

**0913-0916 built a thing to measure with, because the logs could not
settle it.** Three candidates -- the network, the transport in this
player, the decode loop -- and no way to tell them apart. A phone
speedtest said 20.32 Mbps down, which ruled out the WAN and said
nothing about the rest: the C6 is 2.4 GHz only and a phone lands on 5
GHz, so that number was measured on a different radio.

A bulk file download would have been the same mistake in a subtler
form. Bulk transfer runs flat out with large reads and nothing
consuming; streaming here is 2048-byte reads through TLS, an ICY demux,
a 256 KB ring with backpressure, and a decode loop competing for the
same CPU and SDIO link. A healthy bulk number would have been a green
light that meant nothing.

So `bench.c`: connect a real station through `netstream` exactly as
playback would, and read the bytes without decoding them. One variable
removed, everything else identical. **It is a ceiling, not a speed** --
a server that paces itself cannot be made faster by not decoding it, so
SomaFM at 128 reads 128 and that is the server's answer. Only stations
that send as fast as they are taken mean anything; the lossless ones
do, and WNZK has previously outrun the ring.

### What it eliminated

**Not the decode loop.** Drain mean 457-543 kbit/s across runs;
playback on the same station in the same sessions 400-553. Draining
without a decoder is not faster than playing with one. An earlier
543-against-440 comparison looked like a 20% decoder tax and was two
samples of a noisy link.

**Not WPA3 (0921).** `WIFI_FORCE_WPA2` clears `pmf_cfg.capable`, which
is the knob -- WPA3 requires protected management frames, so a station
that does not advertise the capability cannot be given SAE and a
transition-mode AP falls back to WPA2-PSK. `threshold.authmode` is a
FLOOR and does nothing to stop WPA3 being chosen, which is worth
knowing because it reads exactly like the switch somebody would reach
for. Result: 457 mean / 580 peak with it on against 475 / 712 with it
off. No better, at -34 dBm, the best signal of the night.

**Not the RSSI or the channel choice.** The slow network reads -34 to
-41 dBm; the fast one read -53. Both on channel 3.

**Not the read size, PROBABLY, and this one is genuinely unresolved.**
0919 alternates 8192- and 2048-byte reads every second within one
connection, because this network delivered 314 and 543 kbit/s for the
same station minutes apart and a two-press A/B measures the weather.
Two runs disagree: 478 against 485 (a wash) and then 522 against 455 (a
15% gap favouring the large read). **Do not record this as settled.**
If it matters it matters because the co-processor is in compatible
streaming mode with no SDIO SW_AGGR -- one packet per transaction -- so
per-read overhead is paid more often than it should be.

### What it actually is

The link delivers roughly 410-553 kbit/s and WNZK needs 512. The
deficit is about 8%, and the buffer shows it plainly: 14.4s down to
5.9s over ninety seconds while sounding perfectly fine. Nothing is
broken. A station needs marginally more than the link provides.

FLAC is out by a factor of three: 24-bit Ogg FLAC at 48 kHz stereo
wants 1500-1650 kbit/s against a peak that has never exceeded 712. No
amount of code closes that.

**Two-to-one run-to-run variance is the headline finding.** 314 and 543
for the same station, same AP, same signal. Any single measurement on
this network is unreliable, including every one taken before the
benchmark existed -- the "~450 ceiling across four unrelated servers"
that shaped most of an evening was four samples of a moving target.
Measurements that have to be compared belong inside one run.

### What is left, all outside the firmware

- `mempool OOM start (TX)` and `(RX)` have both appeared. That is
  esp-hosted out of transport buffers mid-stream, and the buffer counts
  under `Component config -> ESP-Hosted config` are configurable. There
  is 32 MB of PSRAM to spend.
- SDIO runs at 40000 kHz and the setup docs cap SDIO at 50 MHz.
- `coprocessor=0.0.0` with a major version mismatch, hence compatible
  streaming mode. OTA from the host would end that. Note it cannot be
  the whole story: the 1283 kbit/s peak on the other AP was measured
  with the same mismatch.
- Channel 3 is the worst place to sit on 2.4 GHz here, with four strong
  neighbours on 1 and about twenty on 6. Partial overlap cannot be
  deferred to, only suffered.
- `eh_raw_tp: raw TP inactive` -- it is compiled in and switched off. It
  bypasses the protocol stack and would give a host-to-C6 number with
  no TCP, no TLS and no station pacing, which would say whether any of
  the above can help at all.

### Two unrelated things this turned up

**24-bit streams were refused, not folded (0911).** `netdec` rejected
anything that was not 16-bit on the reasoning that no broadcast AAC or
Opus is anything else. Ogg FLAC is routinely 24-bit, the container
carries it fine, and the fold had existed in `decoder.c` since 0803 --
forty lines away behind a `static`. Moving it to `pcmfold.h` made it
testable, and UBSan found undefined behaviour in it within five
minutes: `(int32_t)(int8_t)src[2] << 16` left-shifts a negative value
on every negative sample, which is to say about half of all audio. It
shipped that way for two years because the file it lived in does not
build on a host.

**A 30-second silence cutoff kills streams that are working.** Both
FLAC attempts ended at `30037` and `30098 ms silent` having decoded
frames and filled the ring the whole time -- they were prerolling
successfully, just slowly, and were cut off for it. WNZK made it twice
at `21362` and `23883 ms` and failed once at 30s, on the same build
minutes apart. At ~500 available against 512 needed, whether there is
audio is a coin flip on network weather.

Also intermittent and unexplained: `first audio bytes look like unknown
(content-type audio/aac)` where the same station sniffs correctly
moments later. It happens during ordinary playback, not just during a
drain -- 0916's commit message blames the benchmark for it and is
wrong.

## Station favourites, blacklist, and the web UI (v0.5.0)

Design settled in conversation, not built. Three states per station --
starred, neither, blacklisted -- with the control on the main page in
the gap between `next` and the moon.

The shape, briefly, because the reasoning is worth keeping:

- **A favourites list per volume, chosen from the radio menu.** It loads
  the same way any other list loads, so `stations_index()`, `next` and
  `prev` keep meaning what they mean today. No mode, no second position
  space -- which was the thing that looked expensive and is not.
- **Two M3U files, not one file with markers.** `favourites.m3u` and
  `blacklist.m3u` beside `stations.m3u`, same SD-first precedence, so
  `stationlist.h` and `stations_append()` are reused unchanged. Three
  states is "in one set, the other, or neither".
- **Unstarring is the one genuinely new write path.** Everything written
  today appends. Removal needs the temp-file-and-rename discipline
  settings.c uses, for the reason stations.h already gives: a list
  truncated by a power cut is worse than no list. This is unavoidable in
  any storage shape -- a three-state toggle where two transitions are
  removals cannot be append-only.
- **Blacklist filters API lists only, in `stations_set_remote()`, before
  the `STATIONLIST_MAX` cap** so the cap counts stations that would
  actually be seen. It must NOT filter the card's own `stations.m3u`: a
  station typed into a file by hand is a deliberate choice, and having
  it vanish because a chart row was blacklisted weeks earlier is a bug
  report waiting to happen.
- **Gold highlight needs `entry_t` to carry a `fav` bool**, resolved
  once in `load_stations()` against the URL from the `stations_get()`
  call already made there -- not per draw. The main-page button does not
  replace this; it is what makes a chart row show gold for something
  already starred.

Two decisions still open: which volume's favourites the highlight
compares against when both are mounted (SD-first precedence is
consistent with everything else, at the cost of a star silently ceasing
to be true on a swap), and how much URL normalisation to do (fold scheme
and host, leave the path byte-exact, matching stationlist.h's habit of
refusing what it cannot handle rather than repairing it).

**The three-state cycle must not pass through blacklist on the way back
to neutral.** Blacklist is the one state whose effect is invisible from
the page it is set on -- it silently shrinks future API lists -- so a
stray double-tap must not reach it. Either tap-toggles-star with
long-press to blacklist, or a cycle where the blacklist step confirms.
The second is preferable; the README already complains about invisible
interactions elsewhere.

**ui.c's row 7 comment is wrong and needs rewriting, not obeying.** It
says "the right-hand side has no such gap: next ends at 521 and the moon
starts at 616, so a sixth control over there would have had to move the
transport". That gap is 95 px. A star centred at 568 with the moon's own
half-span of 40 spans 528-608, clearing by 7 and 8 px -- no tighter than
the gear, which sits in 116-196 against 104 and 199. The comment asserts
no gap exists while describing one the same size as the one it just used.

**THE WEB UI HAS TO MANAGE ALL OF THIS TOO.** `portalweb.c` already
lists stations and adds them; it will need starring, unstarring and
blacklisting, which means the removal path above is reached from two
callers rather than one, and the portal is the side with a real keyboard
and a real screen -- it is where someone will actually curate a
blacklist rather than react to one station. Do not build the on-device
toggle in a way that assumes it is the only writer.

## WNZK declares 320 and costs 512 (measured, 0930)

**The station that sent no `icy-br` now sends one, and it is wrong by
sixty per cent.** From a board log:

    icy-name: WNZK-AM
    icy-br: 320
    first frame: AAC (ADTS), 48000 Hz, 2 ch, 16-bit, 1365 bytes -> 1024 samples

1365 bytes per 1024 samples at 48 kHz is 46.875 frames a second and
63984 bytes a second, which is **511.9 kbit/s**. That is where
netstream's `of 512 needed` comes from, and it agrees to the digit with
the 512 the 0200 probe measured by a different method months earlier.

### Three earlier entries are now wrong, and none of them should be edited

- **0328 said "WNZK sends no `icy-br` either -- not to the PC, and not
  to the device".** True when it was written. The station changed.
- **0328 also said the 512 figure "is 0200's, not the station's", with
  no independent confirmation.** There is one now, from the frame
  arithmetic above, and it is not the station's `icy-br`.
- **netstream.c's 0415 comment ended "on a station that declares
  nothing at all".** Corrected in place in the code, because a comment
  describing the mechanism it sits beside has to be true; the entries
  here are dated records and stay as they were written.

### What it cost on screen

The card preferred `icy-br` over everything, deliberately -- 0404 chose
it so the square showed "the thing to compare against the delivery
figure in netstream's line". With a station that misdeclares, that
comparison inverts: **the card read 320 kbps while the line beside it
read `of 512 needed (81%) SHORT` against a delivery of about 400.** One
screen saying the player cannot keep up with 320 while receiving 400.

The fix is not a return to the decoder's per-frame report, which 0404
rejected correctly -- `last_kbps` latches whatever the last frame said
and moves with the decoder. It is `netstream_actual_kbps()`: bytes in
per second of audio out, on a decoded-audio clock, which netstream has
used as its denominator since 0415 and which nothing outside that file
could read, because there was a setter and no getter.

**A declared bitrate is a claim, and this is the station that proves it
has to be treated as one.** Prefer the measurement everywhere; keep the
declaration as the fallback, because a claim beats a blank line.

### What this does not explain

The reserve still sawtooths and the link still under-delivers: 380-459
kbit/s against 512 needed, reserve peaking at 13.5 s and falling to
1.8 s, `SHORT` on most lines. That is unchanged and is the 0900 series'
problem. Nothing here made the stream work better; it made the screen
stop contradicting itself.

One number worth keeping from the same log: **first sound at 24727 ms**,
against a 30000 ms preroll cutoff. The stream was six seconds from being
killed for being slow while it was in fact working, which is the case
0930's "Stream too slow here" card was written for.

## The 1200 series: cue sheets, USB Ethernet, and the third ring's order

### Two series called 1000 (read this before searching for a number)

**1000–1012 were used twice.** The v0.3.0 work above is the 1000 series
up to 1116. The session that added USB Ethernet and cue sheets numbered
its patches from 1000 again without looking, so there are two of each
of 1000–1012 in the history. The second set, in order:

| # | What |
| --- | --- |
| 1000 | USB Ethernet: `ethernet.c`, `netlink.h`, ASIX + CDC-ECM (RTL8152/8153) |
| 1001 | Ethernet with Espressif's `iot_usbh_ecm` for the Realtek dongles |
| 1002 | Cue-sheet test corpus, 22–26, and `.gitattributes` for the `.cue` bytes |
| 1003 | `encode.sh` writes 22's accented title with `%b` |
| 1004 | `cuesheet.h`, the parser, and `texttest/cuesheettest.c` |
| 1005 | `cuedir.c` and cue playback: "Album.cue#03", spans in `decoder.c` |
| 1006 | `net_route_describe()`: netstream says which interface it went out of |
| 1007 | `esp_usbh_asix` pinned |
| 1008 | route-priority juggling -- superseded by 1009 |
| 1009 | `netlink_pick()`: the default route pinned, not left to esp_netif |
| 1010 | DNS saved and restored around the cable's DHCP |
| 1011 | `esp_audio_codec` held below 2.6, which needs chip revision v3 |
| 1012 | `netplan_should_move`: a stream moves to the cable when it comes up |

Everything after starts at 1200, which was checked free. When a log or
a comment says "1005", it is the cue patch if it mentions cues and the
v0.3.0 one otherwise.

### What the 1200s fixed

- **1200** cue tracks get sidecars (replaygain.c stat()ed the virtual
  path) and neighbours on one sheet never crossfade -- they are one
  recording cut at INDEX 01.
- **1201** the crossfade mixed `s_ring[fill]` as the incoming side. With
  three rings and short tracks the decode can be two tracks ahead, so
  it mixed the wrong track, or -- once fill wrapped to play -- nothing,
  and the rest of the outgoing ring played at a frozen ramp gain. The
  incoming side is `(play + 1) % PCM_RINGS`, and an overlap only starts
  when fill is that ring, because `s_xfade_armed` describes the
  boundary into fill.
- **1202** diagnostics: every writer ring change, and the ring levels
  when the decode starts waiting for one.
- **1203** test file 27, whose tracks draw 3, 2 and 1 bumps -- the
  landmark barely moves in loudness, so 22's tracks all draw the same
  line and cannot show whose envelope a bar is.
- **1204/1205** a track decoded before it is heard kept losing its
  screen commit, because the commit lived in `play_file()` and that
  returns when the DECODE ends. It is saved per ring (`s_late`) and
  applied when the writer arrives. The gate also commits on the writer
  reaching the track's ring, not on a release raised by an earlier
  boundary.
- **1206** the chooser went black after boot: `browser_open()` from the
  decode task raced ui_task's draw, and with every cue sheet parsed the
  load was long enough to lose. `load_dir()` dirties the screen again at
  the end.
- **1207–1209** stars on local files and folders (`starred.c`,
  `starred.m3u` per volume, relative paths, folders end in `/`), the
  panel's star for files (solid / thick gold ring for a starred folder /
  thin white ring), and star requests served from the sliced send loop,
  which is the only loop running while paused. A mark only: nothing
  plays the list yet.
- **1210** the one 1202 found. The decode set `s_ring_fill` to the next
  ring and THEN waited for it. The writer only leaves a ring that is not
  the fill ring, so a ring it was draining became one it could not
  leave: it emptied it, stayed, and played the next track out of it
  ahead of the two already queued. Tracks came out of order and, later,
  the 30 s timeout reset 1.4 MB of a passed-over track. The next ring is
  now chosen, waited for until it is empty AND the writer is off it, and
  only then taken.

**The lesson 1115 already had, again.** Three rings broke two
assumptions that two rings could not: that fill is play + 1, and that a
chosen ring is a free one. Anything that names a ring by `s_ring_fill`
when it means "the next one to be heard" is suspect.

### Still open

- **A cue track starting mid-file costs ~500 ms** before its first
  sample: a block decoded at the top of the file to learn the rate, a
  FLAC seek, a decoder reopen, and decoding forward. 1211 logs the parts
  (`cue: start reached in …`, and `find`/`reopen` on the seek line).
  The likely fix is to take the rate from STREAMINFO at open and seek
  before the first decode. Not audible while a previous track is
  playing, which is every case in the logs so far.
- **Cue sheets are re-parsed on every listing and every play**, all of
  them, which is the repeated `tab5_cue` bursts in every log. A per-
  folder cache would end that and shorten the race 1206 closed.
- **The chooser's rows are filled on one task and drawn on another** at
  boot. 1206 made the result right; the two still touch the array at
  once.

## Where v0.4.0 got to, and the 5000 series

**v0.4.0 is tagged** at the end of the 1200 series above (1211). What it
added over v0.3.0, in the order it will matter to someone picking it up:

- **USB Ethernet** (the second 1000, 1001, 1006-1012): ASIX AX88772 and
  CDC-ECM, so the Realtek RTL8152/8153 dongles through Espressif's
  driver. The cable wins the default route whenever it is usable
  (`netlink_pick()`), the station stays as the fallback, and DNS is
  saved and restored across the cable's DHCP. Confirmed on the board
  with the ASIX; **the Realtek path has not been on hardware yet.**
- **Cue sheets** (1002-1005, 1200, 1203, 1211): a sheet's tracks list
  and play as "Album.cue#03", with sidecars, gapless between
  neighbours, and the image hidden behind its sheet. Test files 22-27.
- **Local stars** (1207-1209): files and folders, per volume, in
  `starred.m3u`. A mark, not yet a playlist.
- **Three rings, properly** (1201, 1204, 1210): the crossfade, the
  screen commit and the ring handoff each assumed the decode was one
  track ahead. Short tracks -- cue tracks, interludes -- made it two.

**Known open at the tag**, carried from the list above: the ~500 ms
before a mid-file cue track's first sample (timed by 1211, not yet
fixed), cue sheets re-parsed on every listing, and the chooser filled
and drawn on two tasks at boot.

### The 5000 series

Patches after v0.4.0 are numbered from 5000; 5000 itself is the
CLAUDE.md change that says so. The jump is deliberate and leaves 1212-
4999 unused, so that a number says which release it came after: 0xxx
and the first 1000-1116 are v0.3.0 and before, the second 1000-1012
and 1200-1211 are v0.4.0, 5000 on is after it.

### 5001 -- four angles, and the square that makes them possible

**The screen turns to all four quarters now, and landscape is the
reason.** The flip was a boolean because 180 is the only angle a
full-width band survives: `gfx_blit()` sends rows, a flipped row is
still a row, and the transfer stays one contiguous copy. A quarter turn
is not -- a logical band of rows is a COLUMN of the glass -- so it was
ruled out, correctly, for as long as nobody wanted landscape.

What changed is where the turn happens. `gfx.c` keeps two extents: the
glass (`s_pw`/`s_ph`, fixed) and the logical one (`s_w`/`s_h`, swapped at
90 and 270). The shadow buffer does not change size, only shape --
720x1280 and 1280x720 are the same allocation -- so an angle change is a
restride and a repaint, not a realloc. Everything that draws keeps
meaning what it meant, including `albumart.c`'s direct writes into
`gfx_fb()`, which is the property that made this affordable.

At 90 and 270 the band is gathered into the scratch the filter and the
180 flip already share, transposed in 16x16 tiles because the source for
one output row is a logical column and a pixel-at-a-time scan of that is
a cache miss per pixel on PSRAM. Rotated bands are split at 240 rows so
the gather fits that scratch. **Landscape is the expensive angle and the
one to measure if the panel ever underruns.**

**The control block is a 720x720 square at every angle**, and that is
forced rather than chosen: the panel is 720 on its short edge, so a
block that is to be the same block in both orientations can be at most
720 square, and if it is to be that in landscape too it must be exactly
720. The artwork gets `1280 - 720 = 560` -- a 720x560 band in portrait
and a 560x720 column in landscape, the same rectangle turned. The rows
inside the square do NOT turn: reading order is top to bottom at every
angle, and the envelope and volume slider are horizontal controls at
every angle. What is shared is the square's extent, the row offsets and
the hit grid.

The square grew from 560 to 720, which bought row 7 a second line. Seven
controls on one row was what 560 px forced, and it put the file chooser
and the sleep page in the same sweep as the transport; the transport is
alone on its row now and the other four sit on one pitch.

**What the layout test caught, that review did not:** prev and next were
moved from +/-112 to +/-132 to clear the new toggle, and +/-132 overlaps
it -- the pill's padded box reaches 98 from the centre and a skip glyph's
reaches 49 back, so the centres must be 147 apart. They are 150 now. The
four aux icons were interpolated across their span, which truncates to
gaps of 206, 207, 207; the pitch is computed once and multiplied now.
Both were written with comments claiming clearances that the arithmetic
did not produce.

**The clocks are ark12 rather than seven segments.** The monospaced
halfwidth cell makes MM:SS fixed width for free, which is the one
property the segments were carrying. What is not free is the width:
these do not zero-pad, so the remaining time is measured and
right-justified rather than offset from `GFX_TIME_W`, and the minutes
are no longer clamped -- `gfx_draw_time()` caps at 99 because it draws
exactly two minute digits, so **a 101-minute track read 99:23**. Hours
are deliberately not a format.

Also: the play/pause disc became a toggle, reporting what the player is
doing rather than what a tap would do; the stream level strip is single
sideband, matching the file envelope, which had been drawn from the
baseline all along; and `settings` carries `screen_rotation` alongside
the old `screen_flipped`, so a rollback lands upright or over rather
than on its side.

### 5002 -- the call site the host suite could not see

5001 renamed `screen_apply_flip()` to `screen_apply_rotation()` and
missed one of its two call sites, so the ESP-IDF build failed on an
implicit declaration in `ui_task`. The host suite passed the whole time
and always would have: **`player.c` does not build on a host**, and
neither do `touch.c`, `settings.c` or `albumart.c`. `texttest` builds
`gfx.c` and compiles `sleeppage.c` for warnings; everything else in a
patch that touches the screen is checked by reading it.

So the rule this cost: **after renaming anything in a file the host
suite cannot build, grep the tree for the old name and count the hits
against the number of edits made.** One edit and two hits is the whole
failure. It is cheap and it is the only check available.

Also here, found while auditing for the same class of mistake: the
play/pause hit box was still `BTN_R`, the radius of the disc the toggle
replaced. The pill is 168x92 and a 92 px box inside it left the 38 px at
each end looking pressable and not being -- exactly the complaint the
old row-7 comment makes about controls whose boxes disagree with their
ink. The box is the pill's now, written out because `in_box()` takes one
half-extent and every other control on the row is square. `BTN_R` is
gone with its last user.
### 5003 -- a clock floor read off the card

The floor's seed is the build timestamp, which is true by construction
and can be months stale on a device that never reaches a network. What
is on the card is often much fresher -- an album copied on last week, a
`stations.m3u` edited on a desktop yesterday -- and those are real
wall-clock readings taken by a machine that knew the time, available
before the radio is up.

`cardtime.c` reads one directory per volume when `storage_generation()`
moves, and offers what it finds to `settings_note_ntp_time()` like any
other claim. It proposes; the floor disposes.

**Only the root, and only one level, because directory mtimes do not
bubble.** A directory's timestamp moves when an entry is added to or
removed from THAT directory, not when a file beneath it changes -- so
adding a track to `/sd/Boa/Twilight/` moves `Twilight`'s mtime and
leaves `Boa`'s and the root's alone. This is emphatically **not** a
change detector and must never be used as one; a card can be rewritten
top to bottom without a single root entry moving. For a floor it does
not need to be: one plausible recent reading is worth as much as ten
thousand.

**The hazard is that the floor is a permanent one-way latch** that
accepts forward jumps without limit -- correct for NTP, where a forged
jump forward only expires certificates early and fails closed. Media
timestamps are foreign input, and FAT can represent 2107. One file
stamped by a camera with a dead battery would put the clock eighty years
ahead, permanently, and every subsequent NTP reply would fail the floor
for the life of the device. So `cardtime_filter()` caps candidates at
`ref + 10 years` and tests that ceiling against the **raw** mtime, so
that subtracting the timezone margin cannot duck a poisoned value under
the bar.

**The margin is subtracted, never added.** FAT stores local wall clock
with no offset and ESP-IDF's FAT VFS converts it through `stat()` as
though it were UTC, so a card written at UTC+14 reads fourteen hours
ahead. A floor a day low is harmless; a floor fourteen hours high
refuses correct NTP for fourteen hours. One day of slack, wider than any
offset on Earth -- the same reasoning `settings.c` already applies to the
build stamp.

exFAT carries a real per-entry UTC offset with a validity bit, which
would make the margin unnecessary where the bit is set. It is applied
anyway: that byte is not exposed through POSIX `stat()`, and reaching it
means going under the VFS to `f_stat()` and `FILINFO`. Noted in the
header so the next reader knows the margin is a limit of the interface
rather than of the filesystem.

`cardtimetest` compiles `cardtime.h` directly, the way `tailplantest`
and `favmatchtest` do. Every decision that could brick the clock is four
comparisons in one inline function, and all four are checked on the host
-- including that the useful case actually works, which is the failure
nobody would notice.

**Also recorded here: the reconcile walk this is not.** A whole-tree
scan through POSIX `readdir()` costs a `stat()` per file, because
`struct dirent` carries no timestamps. FatFs's native `f_readdir` fills
a `FILINFO` with size and date already in it. Anything that later walks
the card for a catalog should go under the VFS for that reason; at a
volume root, bounded by `CARDTIME_SCAN_MAX`, it is not worth the
layering.

### 5004 -- the volume row's icons were never flush

5001 rebalanced the volume groove into three blocks and two equal
gutters, and said in a comment that the outer blocks were flush to the
content box. They were not, and the arithmetic could not have told
anyone: `spk_centre()` and `draw_battery()` derived their centres from
`vol_bounds()` by a fixed 42 px each, a leftover from when the groove's
margins were a flat 96 and the icons sat inside them. Rebalancing the
groove moved the icons with it.

In portrait the output icon landed at 105 with its box at 79..131,
leaving **55 px of dead air** between the content edge and the icon --
precisely the thing the rebalance existed to remove. The battery
floated 47 px off the other end.

**A boot log caught it.** A mute press logged at `x=83`, which is inside
79..131 and nowhere near the 24..76 a flush icon would occupy. Every
piece of arithmetic involved had been checked against itself and agreed
with itself; only the device knew where the icon actually was. `5001`
had added a layout test and that test did not look at this row, so it
passed throughout.

The dependency runs one way now: `spk_cx()` and `batt_cx()` are
expressed against `bar_x0()`/`bar_x1()` and nothing else, and
`vol_bounds()` starts from where they end. The groove itself does not
move -- it was already 147..574 in portrait and still is. Only the
icons do, the output icon left by 55 px and the battery right by 52.

**The mute target moves with it**, from 79..131 to 24..76, and that test
takes no `HIT_PAD_X` by design. A thumb trained on the previous build
will miss low for a while.

`rotatetest` grew the row: no dead air at either end, gutters equal, and
blocks plus gutters covering the content box exactly. The log's own
number is deliberately not asserted -- `x=83` is evidence of the bug,
not of the fix, and pinning it would preserve what it exposed.

`BATT_W` and friends moved up beside `SPK_HALF` because `batt_cx()`
needs them and C compiles top to bottom. That is the second time in four
patches this file has hit that; the first was `fill_rrect()` in 5001.

### 5005 -- closing the sleep page left it on the artwork

Reported from a board, with the log to prove it: `button: close` with
not a single drawing line after it, and the sleep page still occupying
the art square until the next track change nineteen seconds later. The
same press on a different track redrew in 18 ms.

The difference was `visuals_pending`. `play_file()` refused to act on
`s_repaint_art` while this track's visuals were uncommitted --

    if (s_repaint_art && !s_pending_ready && !visuals_pending)

-- on the sound reasoning that a track's cover must not go up before its
audio is heard. `VISUALS_GATE()` exists precisely to stop that, and its
own comment records the eighteen seconds of a previous track's title
that made it necessary.

But that window is not short. With three rings and a track shorter than
one ring's worth of audio, a decode runs tens of seconds ahead of the
writer; the log measured **nineteen seconds** between `playing ... 02 A
Proper Story` and its commit. For that whole window **nothing could
repaint the artwork at all**, so any screen that covered it stayed.

The fix is that `visuals_pending` no longer decides WHETHER to repaint,
only WHAT to repaint. What the screen was showing is `s_shown_path`, the
track `track_commit()` last put up, so that is what a repaint restores.
`path` -- this decode's track -- is still withheld until its commit. The
gate's original job is untouched; it simply no longer takes the repaint
down with it.

Both consumers get it: the main send loop and the paused-send loop,
which is the only loop running while paused and so the only chance a
screen closed there ever gets.

**Not host-testable and not tested.** `player.c` does not build on a
host, this is control flow rather than arithmetic, and the failure needs
a decode running well ahead of playback to reproduce. The reasoning is
above and the board log is the evidence; the confirmation is opening the
sleep page early in a short track and closing it.

### 5006 -- the whole bottom of the menus was an invisible close button

Two faults in one report, and the second is the one that matters.

`sleeppage.c` drew its footer as a 2 px rule and a button, and nothing
else. Everything between them was whatever the page had already drawn
there. In landscape the sleep page needs 860 px of content and has 720,
so the rotation note landed at y ~628 -- inside the bar -- and rendered
on both sides of the centred button. That is exactly the report: "the
rotation description test emerging either side of it". The footer is now
filled `C_BG` before the rule and the button go on, as `panel.c`'s
already was.

The hit test was worse, and both files had it:

    if (y >= h - FOOT_H) { ... return true; }

The entire full-width 120 px strip closed the screen. On the 720 px
portrait glass that is merely generous; rotated, the strip is 1280 px
wide and the 180 px button is 14% of it. The board log shows closes
firing from raw (641,616), (655,172), (667,83) and (663,630) -- four
different places, all mapping to logical y >= 600, none of them on the
button.

Both files now bound the test to `close_box()`, which is also what draws
the button, so the two cannot drift. A footer press outside that box
**sinks**: it returns "handled, do nothing" rather than falling through.
That is not tidiness. In landscape the sleep page's rotation control
spans 526..614 and the footer starts at 600, so a fall-through would
turn the bottom of the bar into a rotation change -- trading an
invisible close for an invisible rotate.

`rotatetest` grew four checks per orientation on the box: centred,
inside the bar, not spanning it, and a press near the bar's left edge
not reading as close. The geometry is duplicated there rather than
included, because neither `panel.c` nor `sleeppage.c` builds on a host.

The overflow that put the note under the bar is still there -- both
files warn about it, and the scroller that fixes it is not this patch.

### 5007 -- a scrollbar for the two settings pages

5006 left the cause of its own bug in place: the sleep page is 860 px of
content and landscape gives it a 504 px viewport, so the sleep timer --
the last control on it -- could not be reached at all. The panel's row
tabs are 888 px at twelve rows and had the same problem. Both files
warned about the overflow in the log and neither could do anything about
it.

**Why a bar at the edge and not a drag on the content.** Both pages are
mostly horizontal sliders whose drag begins anywhere in the row. A
content drag has to decide, within the first few pixels of a gesture,
whether a finger moving down and left means "scroll" or "dimmer", and it
would be wrong often enough to be noticed. browser.c reached this
conclusion already and this is its idiom, in pixels rather than rows
because these pages have controls of five different heights.

`menuscroll.h` is the whole mechanism: header-only and arithmetic-only,
like `cardtime.h`, so `menuscrolltest` compiles the real thing and there
is no second copy to drift. A press anywhere in the strip CENTRES the
bar on the finger rather than keeping the grab point, which makes
press-anywhere and drag one gesture -- browser.c's choice, and the
reason a jab at the bottom of the track goes to the bottom of the
content instead of nudging by a bar's height.

**There is no clip in gfx, so the pages clip by overdraw.** Content is
laid out from `list_top()` as though the page were unbounded and drawn
shifted up; then the header is drawn, then the footer, both opaque. That
is why the header moved from the first thing each draw does to nearly
the last. 5006's filled footer was half of this already.

**One subtraction, not many.** Every box on both pages chains off
`list_top()` -- `brightness_box()` used to compute from `LIST_TOP`
directly and now asks `screen_box()` -- so the scroll enters the layout
once. `panel.c` measures its content rather than predicting it: the
draws already returned where they ended, because two of them were
computing it to warn about overflow.

Presses are gated on the viewport (`in_view`). A drag already running
follows the finger anywhere, but a press does not START a slider on a
row that is scrolled under the tab strip, which is what would otherwise
happen to anything behind the bar.

**The test found the strip's width.** 40 px was the first guess;
`menuscrolltest` asserts the strip clears the sliders, and the sliders'
knobs reach to 38 px from the right edge, so 40 would have turned "drag
the brightness to full" into "scroll". It is 32.

Neither page builds on a host, so both were additionally checked with
`gcc -fsyntax-only` against `texttest/shim.h` and a directory of empty
stubs -- the cheap half of the lesson in 5002, which is that a file the
suite cannot build is a file where a rename compiles in the author's
head and nowhere else.

### 5008 -- the overflow warning was measuring the scroll

5007 broke this without editing the line. Both tab draws ended with

    if (used > gfx_h() - FOOT_H)

where `used` is an absolute y. Once the page scrolled, `used` moved with
it, so the comparison measured where the content currently SAT rather
than how tall it was. A board log caught it in one drag on the NET tab:
1068 px, then 958, then 743, three readings of a tab whose height had
not changed, one per redraw. In content coordinates it is 964 at every
scroll position -- and `s_content_h` was already computing exactly that,
correctly, six lines away.

The check is now panel_draw()'s, which is the only place that holds the
content height, and it compares against the viewport rather than the
glass.

It is also no longer a warning. Its stated purpose was that the next
person to lengthen a note would find out here instead of finding text
under the CLOSE button; 5007 means they find a scrollbar instead. A tab
taller than its viewport is now a fact about the tab, so it logs at INFO
-- and only when the height or the tab changes, rather than on every
one-second refresh, which is what made three lines out of one gesture.

The general shape of this is worth keeping: adding a coordinate
transform does not announce itself at the places that assumed there
wasn't one. The two lines that broke were the two lines in the file
already thinking about the footer, and neither was in 5007's diff.

### 5010 -- the media index's order, and its reconcile step

5009 was `MEDIA-INDEX.md`, the plan. This is its first code, and it is
the part with no I/O: `main/mediaindex.h`, header-only, which nothing
calls yet. **Host-tested, not built, not flashed.**

It holds the two rules that fail silently. Neither one crashes when
wrong. A wrong order tombstones a folder and adds it back, re-reading
every tag, on every walk. A wrong step appends a line per dead file per
walk, for ever. Both are checked in `texttest/mediaindextest.c`.

**The order is not strcmp.** A reconcile is a merge-join of the index
against a depth-first walk that sorts each folder, and it is only
correct if both sides arrive in one order. Whole-path `strcmp()` is
not the walk's order: `' '` is 0x20 and `'/'` is 0x2F, so strcmp puts
`a b/y.flac` before `a/x.flac`, while a walk visits folder `a` before
folder `a b`. `midx_path_cmp()` sorts `'/'` below every byte except the
terminating NUL, which makes comparing whole paths the same as comparing
component by component. Within one folder it is plain strcmp, since a
name holds no `'/'`.

The walk must sort with this, not with the chooser's `cmp_entry()`,
which puts folders first and ignores case -- right for a screen and
wrong for a merge.

**The step** is `MEDIA-INDEX.md`'s five cases plus the one it left
implicit: a path already tombstoned and still absent is KEEP, not
BURY, so its time of death is not moved to every walk's "now" and the
tombstone can one day age out. A tombstoned path back with a different
stamp is UPDATE, not REVIVE, because its old tags describe another
file. The stamp is mtime AND size, since each alone misses something
ordinary.

**Disorder stops the merge.** `midx_in_order()` is checked on every
element taken from either side. By the time disorder is visible a wrong
tombstone may already be written. That is survivable only because a
tombstone revives without a tag read, which is the reason they are kept.
A walk that cannot finish a folder must also stop rather than hand over
a partial listing, since everything under it would come back BURY.

**Mutation-checked**, seven deliberate bugs:

| mutation | result |
| --- | --- |
| `'/'` compared as its own byte | 88 failures |
| a tombstone buried again when still absent | 56 failures |
| REVIVE re-reads tags | 84 failures |
| stamp compares mtime only | 43 failures |
| `midx_in_order()` accepts a repeat | 1 failure |
| bytes compared signed | 465 failures |
| names sorted by strcmp | passes -- equivalent by construction |

The walk check also counts the adjacent pairs strcmp would have got
backwards, and fails if that count is zero, so the corpus is shown to
reach the trap instead of passing by never meeting it.

**Decided, recorded in `MEDIA-INDEX.md`**: cue tracks rather than
sheets; one merged library with SD preferred; a derived search file;
a 128-byte index record keyed by path prefix; rebuild on a version
bump. Still the walk's to decide: which stamp judges a cue track stale.

**Noted, not done: `jsonpick.h` could become cJSON.** Its header and
commit say it exists to keep a JSON parser off the P4. cJSON was
already on the P4 when it was written -- `settings.c` and `replaygain.c`
through ESP-IDF's `json` component, two weeks earlier. It stays for
now: it works, its refusal cases are tested, and it allocates nothing.
cJSON allocates one node per value, and with no `cJSON_InitHooks` that
lands in internal RAM, which is the scarce heap right after a TLS
fetch. The replacement is due when radio-browser needs a second field
or a nested one. That is when to route cJSON to PSRAM through the
hooks, a global change that also moves settings and the sidecar, so it
should be made on purpose.

### 5011 -- the index record, and finding things in it

The fixed-width file `MEDIA-INDEX.md` asks for, added to
`mediaindex.h`, still with no I/O. Records come in through a `read`
callback and long paths through a `fullpath` callback, so the host test
runs them on an array and the device will run them on fseek and fread.
**Host-tested, not built, not flashed.**

**128 bytes, little-endian, packed byte by byte:** a 104-byte path
prefix, the path's length, a dead flag, a reserved zero byte, the
catalog offset, then mtime and size. The key is 104 bytes rather than
the 120 that was discussed, because the stamp is in the record too. With
the stamp in the index, reconcile merges two files it reads in order,
the walk and the index, and touches the catalog only to append. 20 000
tracks is 2.5 MB.

**A path longer than the key is still found exactly.** When a query and
a record agree on all 104 bytes, the record's full path is read from
the catalog and compared. The host test found one case where that read
isn't needed: a query exactly 104 bytes long is a proper prefix of the
longer record, so it sorts first. It is now decided without the read, so
no path of 104 bytes or fewer ever touches the catalog during a search.

**A folder listing costs one search per child.** Everything under
`a/b/` is one contiguous run in path order, so `MIDX_PAST_PREFIX` jumps
over a listed subfolder in log2(n) reads. The test's box-set folder
(three discs, 600 tracks) lists in fewer reads than reading through it.
A root with hundreds of small folders does not gain from the skip, and
the test does not claim it does.

**What a bad catalog can and cannot do.** The full path that comes back
is checked against the record's length and key, and a mismatch sets
`err`: the caller should stop trusting the index and rebuild it. A line
that matches both and differs after byte 104 -- a sibling with the same
long prefix and the same length -- can't be caught this way. The rule
that prevents it comes before any search: a catalog offset is only
valid for the catalog the index was built against, so whatever compacts
the catalog rebuilds the index in the same step.

`midx_rec_unpack()` refuses anything the packer could not have written
-- a reserved byte, an unknown flag, a NUL inside the key, a byte in
the padding, a zero or oversized length, a leading slash -- because
that is what a torn write or someone else's file looks like, and the
answer to all of them is the same: rebuild.

The path limit is 506 bytes: the player's 512-byte buffers, less the
NUL, less `/usb/`.

**Mutation-checked**, nine bugs, all caught: the 104-byte shortcut
removed; the catalog's key not checked; PAST_PREFIX behaving as AT;
AT off by one; padding not checked; the length stored in one byte; a
long prefix matched on the key alone; mtime stored in 32 bits; a path of
exactly 104 bytes treated as a long one. Two of those originally made
the listing loop in the test spin for ever rather than fail; it is
bounded now, so a broken skip fails CI instead of hanging it.

**Next.** Reconcile can write the new index as it goes. The merge
produces the index's records in index order -- a KEEP copies the old
record, anything else writes a new one pointing at the line it just
appended -- so the new index is streamed to a temporary file and
renamed over the old one at the end, with no sort anywhere. A reconcile
that stops partway throws the temporary file away. The lines it has
already appended are then orphans: harmless, and dropped by the next
compaction.

### 5012 -- the catalog's lines

`main/mediacat.{h,c}`: the JSONL catalog the index points into.
`.defeatist.cat` at the volume root, beside `.defeatist.dat`. One line
per record, appended; a path's latest line is its record, and the index
says which line that is. **Built into the firmware and called by
nothing yet. Verified on the host, but not by `texttest` -- see below.**

**A line** is readable words, as `settings.h` argues for anything on a
card people put in a computer:

    {"format_version":1,"path":"Artist/Album/01 Song.flac",
     "mtime":1735500000,"size":8760320,"title":"Song",
     "artist":"Artist","album":"Album","written":1789084800,"clock":"s"}

plus `deleted_at` on a tombstone. Empty tags are left out, since an
untagged library would otherwise spend most of its catalog on `""`.
`written` and `deleted_at` come from `settings_now()`, and `clock`
records whether that was the floor or a synced clock. A key this build
doesn't know is skipped; another `format_version` isn't read at all.
Decoding is strict in every other way: a string too long for its field
is refused, not cut, because a cut path is a different path.

**Appending, where `replaygain.c` gave it up.** The difference is
worth recording because the two files look alike. A sidecar holds
one record, so each appended line repeated the one before it. The
catalog holds thousands, and a line is one of them.

**A torn last line is closed off before the next append.** If a card
is pulled mid-append, the file ends without `'\n'`. The next append
would then join its line to the torn one, and both would fail to parse:
one lost write would cost the next one too. `mediacat_append()` reads
the last byte and writes a `'\n'` first if it's missing. The same gap
exists in `settings.c`'s `write_file()` -- a failed append there is
followed by a record glued to it, which is lost until the save after
that. Not fixed here; noted.

**cJSON is exact to fifteen digits, not to 2^53.** A double holds
integers exactly up to 2^53, but cJSON prints numbers with `%1.15g` and
falls back to `%1.17g` only when the text reads back different -- and
it tests "different" with a relative epsilon. 2^53 prints as
`9.00719925474099e+15`, passes that test, and reads back 2 short. So
every number in a line is refused past 10^15 - 1 on the way in. That
limit is a petabyte for a size and thirty million years for a time;
what matters is that nothing is silently written as a different
number. Found by the local test on its first run.

`cJSON_PrintPreallocated()` into the module's static line buffer
instead of `cJSON_PrintUnformatted()`: this runs once per track on a
walk, and the print allocation is the one that can be avoided. The tree
itself still allocates a node per field, which lands in internal RAM
(5010's jsonpick note); a walk should be measured for that.

**Not in `texttest`.** The host suite runs in CI before ESP-IDF is
installed, and cJSON comes from ESP-IDF's `json` component, so there is
no cJSON for it to link. A test that only runs on one machine is the
thing this file has complained about before, so none is committed. The
checks were run locally against cJSON 1.7.18 under ASan and UBSan,
with `mediacat.c` itself compiled at `-O2 -Wall -Wextra -Werror`:
round trips of plain, hostile (quotes, backslash, newline, control
bytes, invalid UTF-8) and worst-case records; seventeen malformed lines
refused; an unknown key skipped; appends returning growing offsets with
the lease balanced; a torn line closed off; reads at a mid-line offset,
at a torn line and past the end refused. Putting that in CI would mean
fetching cJSON for the host build, pinned the way `cmake/vendored.cmake`
pins minimp3 -- a decision about CI, left for the owner.

### 5013 -- the walk

`main/mediawalk.{h,c}`: every playable track on one volume, offered to
a callback in the media index's order, with its stamp. This is the card
side of a reconcile. **Built into the firmware, called by nothing yet,
and tested on the host against a real directory tree
(`texttest/mediawalktest.c`).**

The walk owes the merge two things a listing doesn't.

**Order.** Depth-first, with each folder sorted by `midx_name_cmp()`,
so the whole walk is strictly increasing under `midx_path_cmp()`. It is
not the chooser's order, which puts folders first and ignores case.

**Completeness, or nothing.** A reconcile reads a path missing from the
walk as a deleted file and buries it. So a folder that can't be read to
the end fails the walk (`MWALK_FAILED`) instead of coming back shorter.
There is no truncating cap here like the chooser's 512 or the
playlist's 1024. The one cap, `MWALK_DIR_MAX` (8192 entries), fails.
`readdir()` returns NULL for both "end" and "error", so errno is
cleared before each call and checked after.

What is left out is left out the same way on every walk, so it can't
flip between them: dotfiles and `._` sidecars, anything
`decoder_supports()` refuses, audio a cue sheet covers, folders deeper
than 24, and paths longer than `MIDX_PATH_MAX`. Each is logged.

**A cue track's stamp is the sheet's and its audio's together**: the
later mtime and the summed size. Which tracks a sheet keeps depends on
the audio's length, so a re-ripped image under an untouched sheet has
to count as a change. That needed one accessor on `cuedir`,
`cuedir_audio()`: which file in the folder track *i* plays from. That
is the whole of this patch's change to existing code.

Names go into one PSRAM arena per open folder instead of one `strdup`
each: a few thousand small allocations would land in internal RAM.
Everything is static or heap, and the folder stack is a static array,
so the walk uses a fixed, small amount of task stack at any depth. One
lease per `opendir`, `readdir`, `closedir` and `stat`, never across the
walk and never across the callback.

**The test found a bug on its first run:** an empty folder reached
`qsort(NULL, 0, ...)`, which is undefined however small the count.

`mediawalktest` compiles the real `mediawalk.c` and runs it with real
`opendir`/`readdir`/`stat` over a tree it builds in `/tmp`: the `a/`
versus `a b/` trap, uppercase, `_`, UTF-8, dotfiles, `._` files,
`.Trashes`, non-audio, a sheet covering an image, a sheet with no
audio, an empty folder, a tree exactly 24 deep with a file one level
below it, and paths at `MIDX_PATH_MAX` exactly and one byte past it.
It checks the walk is exactly the expected list in order; the cue stamp
moves when either file does; two walks of an unchanged tree agree; a
stop is a stop; a missing mount and a folder of 8193 files fail without
offering anything past the failure, while a folder of exactly 8192 is
fine; and the lease is never nested, never held across a callback, and
always returned. What is stubbed is the ESP-IDF surface: the arbiter,
`storage_is_hidden()`, `decoder_supports()`, and `cuedir`, whose real
version needs the decoder. The fake follows `cuedir.h`'s contract.

**Mutation-checked**, nine bugs, all caught: no sort; truncating at the
cap; cue stamp from the sheet only; depth off by one; path limit off by
one; dotfiles not skipped; covered audio not hidden; an unreadable
folder skipped instead of failing; `stat` keeping the lease.

**Left as it is, and worth knowing.** `cuedir`'s own `read_names()`
stops at 1024 names without saying so, so in a folder bigger than that
a sheet can fail to hide its image, or fail to resolve its FILE line.
It does so the same way every time, so the index stays stable, but the
image would be indexed as a track. And `cuedir_load()` returns NULL on
no memory as well as on "no sheets". A walk that hits that shows the
images instead of the tracks, and the next walk flips back. Both belong
to `cuedir` and the chooser shares them; not changed here.

### 5014 -- the reconcile

The pieces from 5010-5013, joined. `main/mediasync.{h,c}` is the engine
and `main/medialib.{h,c}` binds it to the card. **Built into the
firmware, called by nothing yet. The engine is tested on the host
(`texttest/mediasynctest.c`); the binding is not.**

**The new index is written as the merge goes.** The merge visits paths
in index order, and each step yields that path's new record: a KEEP
copies the old one, and anything else points at the catalog line it
just appended. So the new index streams to `.defeatist.ixn` with no sort
and no table in memory. When the merge completes, the old index is
removed and the new one renamed over it.

**Anything short of completion changes no index.** A stop, a failed
walk, disorder on either side, or a catalog that refuses a line: the
temporary file is removed and the old index stands, byte for byte.
Catalog lines already appended are then orphans, pointed at by
nothing. And an incomplete walk buries **nothing**, not even as orphan
lines: it hasn't looked at what it didn't reach. The first version of
the host test missed that, and a mutation proved it (below).

**A damaged old index** -- a record `midx_rec_unpack()` refuses, a size
that isn't whole records, disorder, or a long path the catalog doesn't
confirm -- fails the run and removes the index, so the next run
rebuilds from nothing. That is the one answer to an index that can't be
believed. The window between removing the old index and renaming the
new one leads to the same rebuild.

**Fallbacks for a missing catalog line.** REVIVE carries over the
tombstone's tags; if that line is gone, the file is read as new. BURY
copies the live line; if that is gone, the tombstone is written from
the index's own path and stamp, which is all a tombstone needs.

**The version is in the file name,** `.defeatist.ix1`. A format change
renames the file instead of migrating it, which is the rebuild
`MEDIA-INDEX.md` settled on, and the build that bumps the name removes
the old file.

**The binding.** Catalog appends through `mediacat_append()`. Catalog
reads open the file per read, because they are the minority (long
paths, revives, buries), and a handle held open across the run would be
a second view of a file being appended to, with its own cached size and
sector. Tags are read the way the screen gets them: a cue track's from
its sheet with `cuedir_tags()`, anything else's with
`covertag_read_tags()`. Times come from `settings_now()`, marked
synced or floor by a new `wifi_ntp_synced()`, which latches true when
`on_sntp_sync()` accepts a reply.

**`MAX_OPEN_FILES` 5 to 8, per volume.** Playback holds up to three
files (decoder, art reader, chooser folder). A reconcile holds the old
and new index for its whole run, plus briefly a folder, sheet, tag or
catalog file -- six, and seven when those overlap. The sum is written
next to the number in `storage.c`.

**Host test.** `mediasynctest` runs the real `mediasync.c` on real index
files in `/tmp`, with an in-memory catalog, tag reader and walk: 600
tracks including long paths and the `a/` versus `a b/` trap. It covers
a first run that adds everything; an unchanged run that writes no line,
reads no tag, reads the catalog only for the long paths, and leaves the
index byte-identical; removals, changes and additions counted exactly,
with tombstones flagged and carrying the run's time; a rerun that
buries nothing again; the original card back with revives that keep
their tags and read none; a failed walk, a stop, disorder and a refused
append each leaving the old index byte-identical, no temporary file and
no tombstone; a tombstone whose live line is gone; and three kinds of
damage each removing the index and rebuilding.

**Mutation-checked**, eleven bugs, all caught -- two of them only
after the test was fixed. "Bury after an incomplete walk" survived at
first because nothing gets installed on failure. The test now also
requires no tombstones from a failed or stopped run. "Long path
unconfirmed" survived because the corruption I planted also broke the
order, so the order check caught it first. The test now plants one
that still sorts in place. Two more crashed the test instead of failing
it, because the test read back an index without checking it was there.
Fixed.

**Not yet known, and worth measuring when something calls this:** how
long a first run takes on a real card. A cue track's tags come from
`cuedir_tags()`, which parses its sheet and probes its audio's length
for every track. An image of twenty tracks is twenty parses, which is
fine for FLAC and could be slow for an MP3 without a Xing header.

### 5015 -- a REINDEX button

The first caller of 5014. The SD and USB tabs gain an `index` row and a
**REINDEX** button under it, shown only while that volume is mounted.
Automatic reindexing on mount comes later and will call the same
function. **Built on the host as far as the fakes reach; not flashed.
This is the first of the media-index patches that does anything on
the device.**

**The button only asks.** `medialib_request()` makes a task, "reindex",
priority 1 like media_task, and returns. The press is on ui_task, the
one writer of the framebuffer. A run is a walk of the whole volume plus
a tag read per new track, and it doesn't belong on either the UI task
or the player task: the benchmark button's pattern, a flag picked up by
`player_loop()`, would block playback for the length of a first run.
The task is made per run and deleted after, so its stack is held only
while there is work.

**8 KB, and it reports its own headroom.** The engine, walk and catalog
keep their buffers static. What goes on the stack is the tag path --
`cuedir_track()` and `sheet_load()` hold about 2.5 KB of path buffers,
covertag about 1 KB -- plus printf. That is an estimate, so every run
ends by logging `uxTaskGetStackHighWaterMark()`, and the first run on
hardware says whether it holds.

**One run at a time, on either volume,** because the engine's state is
static. While one runs, the other tab's button is greyed and reads
BUSY; the running tab's reads INDEXING. The row shows the live count
("indexing, 812 so far") and then the result: "4210 tracks (+12 ~1
-3), 41 s", "unchanged", "stopped: the volume went away", or "failed".
The panel's one-second refresh keeps the count moving.

**A second hold slot.** A run holds files open, and `storage_hold()` is
what defers an unmount while that is true. But it is one slot, and the
player sets and clears it on every track change, so a reindex that
took it would be released by the next track. `storage_hold_background()`
is a slot of its own. Either hold now defers the SD unmount and makes
`storage_usb_busy()` true -- so the USB power switch reads IN USE and
refuses to turn off while a drive is being indexed, as it does while a
track plays from it.

A pulled card is marked absent and its unmount deferred, as before. The
walk sees it at the next track: `medialib.c` wraps the engine's
callback in a check of `storage_present()`. The run stops -- stopped,
not failed, so nothing is buried and the old index stands -- closes
its files, and releases, and the next poll unmounts. A USB drive that is
unplugged detaches whatever holds it, as it always has; the walk then
fails on its next read.

**A correction to 5014's last paragraph.** It worried that a cue
image's tracks could be slow to tag for an MP3 without a Xing header,
because `cuedir_tags()` probes the audio's length per track. But
`duration_probe()` has no MP3 branch at all -- it handles FLAC, Ogg,
WAV and MP4, and returns 0 for anything else -- so an MP3 image costs a
sheet parse per track and no scan. The per-track re-parse is still
there, and is still the thing to measure.

### 5016 -- one catalog handle per run

**The first measurements, on hardware, with 5015** -- the baseline the
next two patches are measured against:

| | tracks | time | per track |
| --- | --- | --- | --- |
| microSD (SD8G), first run | 52 added | 3993 ms | 77 ms |
| USB (0781:5571), first run | 1192 added | 50067 ms | 42 ms |
| USB, unchanged rerun | 1192 kept | 8152 ms | 7 ms, 103 catalog reads |

The reindex task's stack never went below 3456 bytes free of 8192, on
the SD run with its cue sheets; the USB runs left 5196 and 5504. 8 KB
stands.

**Every append opened, flushed and closed the catalog,** 1192 times on
a first run. Every catalog read -- one per path longer than the index
key, 103 on that drive -- opened and closed it too. On USB, metadata
writes are the expensive part of this program's I/O (the sidecar's
temp-and-rename measured 1.3 s for three), and a first index was paying
for them per track.

**`mediacat_session_open()` / `_read()` / `_close()`**: one `"a+"` handle
for the whole run. Appends go through it buffered, with no open, flush
or close per line. The offset is tracked, not asked for: `ftell()` on a
buffered append stream is a flush. A write that fails refuses every line
after it, since the tracked end and the real one then disagree. Reads go
through the same handle, which also retires 5014's reason for opening
per read: that was about keeping a second, staler view away from the
appender, and with one handle there is no second view. `mediacat_append()`
outside a session behaves as before. The open-and-close-the-torn-line
step is shared between the two, which is why the function is
restructured rather than extended.

**The rule that comes with buffering: the catalog is flushed before the
index goes in.** The new index points at lines that, in a session, are
only in a buffer until the close. An index installed first and a
battery lost second would point past the end of the catalog. The
engine gets an optional `cat_flush` hook, called after the merge and
before `remove` and `rename`. False fails the run and installs nothing.
`mediasynctest` checks the hook *when* it runs -- the temporary index
exists and the old one is still byte-identical at that moment -- that
it runs exactly once on a run that installs and never on one that
doesn't, and that a failed flush leaves the old index in place. Three
mutations (never called, result ignored, called after install), all
caught.

**The summary gains a second line**: milliseconds in tag reads, in the
catalog, and the remainder in the walk and the index. The last log
could only support a guess about where 50 seconds went; the next one
will say.

`mediacat`'s session code was checked locally against cJSON 1.7.18, as
5012's was, and the same CI note applies: 67 checks, including appends
read back through the session before the flush, an append after a read,
a second session refused, and a torn tail closed off at open.

### 5017 -- a cue track's tags from the sheet the walk has open

The SD log from 5015 shows the other half of a first run's time: every
cue track's tags came from `cuedir_tags()`, which re-reads the folder,
re-parses the sheet and re-probes every audio file the sheet names --
per track. `26 cue-malformed.cue` was parsed three times for its three
tracks. `25 cue-multifile.cue` probed its three files for each of its
three. About 60-100 ms a cue track, most of the SD run's 4 seconds,
while the walk had that very sheet loaded in `lv->cues` the whole time.

**`cuedir` rows now carry their track's tags**: number, title,
performer and the sheet's title as the album, copied from the parse
that already happens in `cuedir_load()`. `cuedir_row_tags()` returns
them cut on a character boundary and defaulted to "Track N" in exactly
the way `cuedir_tags()` does, so the two can't disagree. 384 bytes more
per row, in PSRAM, only while a folder's sheets are loaded.

**`mwalk_cue_tags(path, ...)`**: the tags of the cue track the walk is
offering *right now*. It answers only inside the callback and only for
the path the callback was given; anything else is false, and
`medialib`'s tag step then takes the long way. The pointer to the
current entry is set before the callback and cleared after it. The
mutation that leaves it set is caught by ASan as a use-after-free of the
folder that the walk has since freed, which is the hazard the clearing
prevents.

`mediawalktest`'s fake `cuedir` gains row tags. The callback checks that
each cue track gets its own row's tags, a plain file gets none, a path
other than the one offered gets none, and nothing is answered after the
walk. Four mutations, all caught.

### 5018 -- reindex on mount

`medialib_poll()`, called once per pass of ui_task's loop. That loop
runs whether or not anything is playing -- the player's idle loop,
where `cardtime` and the station files notice a mount, does not, and a
drive plugged in mid-album would have waited for the album. The poll
costs one compare of `storage_generation()` when nothing has changed.

A volume that has newly appeared is **due ten seconds later**
(`MEDIALIB_SETTLE_MS`). Boot is the busiest the card ever is --
settings, the resume track, the chooser and the playlist, cue sheets
parsed for each of them -- and the USB drive mounts a second or two
behind the SD. The index can wait for all of that, and a card pushed in
and pulled straight out never starts a run only to stop it.

**Once per mount, one at a time.** With both volumes present at boot
the SD goes first and the USB starts when it finishes. A run that fails
is not retried until the volume is mounted again or REINDEX is pressed,
so a card that can't be indexed isn't walked over and over (the
stations-file loop that re-read a missing file every 104 ms, recorded
above, is the pattern avoided). A press of REINDEX during the wait
replaces the automatic run instead of being followed by it. During the
wait the index row reads "starts in a few seconds".

It runs during playback. The reindex works under the BACKGROUND
class, one lease per operation, which is what the arbiter exists for;
whether a first index of a large USB drive under playback is audible
is a question for the next log.

### 5019 -- stamps from the listing, not a stat() per file

**The 5018 log**, both runs automatic, both indexes already built:

| | tracks | total | tags | catalog | walk + index |
| --- | --- | --- | --- | --- | --- |
| microSD, unchanged | 52 | 681 ms | 0 | 24 ms, 9 reads | 657 ms |
| USB, unchanged, under playback | 1192 | 9974 ms | 0 | 496 ms, 103 reads | 9478 ms |

While the USB run went on, the Bastion track playing from the same
drive saw a worst playback wait of **5 ms** and prefetch 21 ms, with no
underrun. The index held the drive for 30% of the track, in 5834 leases
of at most 49 ms each. That is the arbiter doing what it was built for.
The task never went below 4320 bytes of stack free.

**So an unchanged pass is almost all walk: about 8 ms a track, and
nearly all of it is `stat()`.** POSIX `readdir()` gives a name and a
type, so the walk took each file's size and date from `stat()`. On FAT,
`stat()` finds a name by reading its folder from the top. A folder of a
hundred tracks is read about a hundred times, and `undertale` on that
drive is 101. `MEDIA-INDEX.md` said this before any code existed.

**`mediadir.{h,c}`: one `f_readdir()` pass per folder**, straight from
FatFs, keeping each entry's `FILINFO` size and date and time words.
ESP-IDF's own `readdir()` is that same `f_readdir()` into a `FILINFO`,
copying out only the name, so the names are byte-for-byte what the
chooser sees. FAT32 and exFAT alike: with `FF_FS_EXFAT` the size is a
64-bit `FSIZE_t`, and FatFs gives exFAT's times in the same two words
(its 10 ms and UTC offset are dropped). `mediadir.c` was syntax-checked
against IDF v5.5's `ff.h` with exFAT off and on.

**The right drive, exactly.** FatFs paths are `N:/...`, and IDF does not
publish which N a mount got. `storage_mark_hidden()` tries each one,
which is fine for an attribute and wrong for a walk: both volumes can
have `Artist/`. `storage_ff_drive()` asks IDF for the SD's drive by its
card (`ff_diskio_get_pdrv_card()`). The USB drive's number is inside
`msc_host_vfs`'s opaque handle, but with `FF_VOLUMES` at 2 it is the
other registered drive, and `f_opendir()` on an unregistered one fails
with `FR_NOT_ENABLED` before touching a disk.

**Sheets and covered audio stay in the folder's entry list, marked**,
so a cue track's stamp (sheet plus audio) comes from the same listing
through a binary search, not two `stat()`s. The walk no longer calls
`stat()` at all.

**`midx_fat_time()`** turns the date and time words into seconds since
1970, read as UTC, by arithmetic (Hinnant's `days_from_civil`). No
`struct tm` or `mktime()`, so nothing about TZ or `tm_isdst` can move
it. `mediaindextest` checks it against the host's `timegm()` for every
valid FAT date, 1980 to 2107 -- all 46 751 of them -- plus both extremes (checked
independently), two-second resolution, and corrupt fields that still
yield a distinct stamp no real date can produce.

**Why the index is renamed `.ix2`.** ESP-IDF's `stat()` gets its mtime
from `mktime()` of the same fields, which agrees with this while TZ is
unset. Relying on that to the second, for every file and forever, is
the kind of agreement that holds until it doesn't, and it would fail
silently as a reindex that re-tags everything. So the stamp's source is
treated as a format change: the index is now `.defeatist.ix2`,
`MEDIALIB_OLD_INDEX_NAMES` lists `.ix1`, and the first run of this
build removes it and rebuilds. That first run is the first-index
measurement 5016 and 5017 were waiting for.

**The catalog grows by a full set of lines on that rebuild.** The old
lines are orphans that nothing points at. Nothing compacts the catalog
yet; that is a task still to do, and it has to rebuild the index in the
same step (5011).

**Walk test.** `mediawalktest` supplies `mdir` over POSIX, skipping `.`
and `..` as `f_readdir()` does, and can now make a folder fail partway
through. That is the read error the walk must never take as the end of
a folder, and the POSIX `readdir()` errno case could never be reached
on the host. Five mutations, all caught: a read error taken as the end,
covered audio offered, a sheet offered, the cue size from the sheet
alone, and the lookup searching the wrong way.

### 5020 -- the include 5019 put in the wrong place

5019 failed to build: `diskio_sdmmc.h` names FatFs's `BYTE` without
including `ff.h`, and 5019 had included it above `ff.h`, in sorted
order. It now comes after `ff.h`, with a comment saying why it's out of
order. The host checks for 5019 compiled `mediadir.c` against the real
`ff.h` but never compiled `storage.c`, which needs the SDMMC driver. The
error reproduces on the host against IDF v5.5's own headers in the old
order, and the new order is clean.

### 5021 -- a crossfade across a rate change

Crossfades used to stop at every sample-rate change. The two rings the
writer mixes have to hold the same rate, and there was no resampler, so
the decode loop disarmed the fade, drained the outgoing ring, moved the
I2S clock and played the dip instead (0719 and after). On a mixed
library, 44.1 kHz files next to 48 kHz Opus, that was a lot of the
boundaries.

`rateconv.c` wraps `esp_ae_rate_cvt` from `espressif/esp_audio_effects`,
the same repository and licence as `esp_audio_codec`. It is pinned
below 1.4 for the same reason: 1.4 needs P4 silicon revision 3.0, and
these boards are v1.3. The library is a Blackman-windowed polyphase
FIR, and its call chain uses under a kilobyte of stack.

**What it does.** At a track's first block, if a crossfade is armed, the
outgoing track is still queued and the new file's rate differs from the
output's, the new track is converted to the rate the output is ALREADY
running at. Its ring is labelled with that rate. The dip, the drain and
the reconfigure all stand down (`carry`). The writer then sees two rings
at one rate and runs the ordinary crossfade. None of the writer's code
changed.

**When it stops.** The next track at the same file rate continues the
conversion with its filter state kept, so a gapless album that was
crossfaded into stays gapless. The first boundary that is neither a
crossfade nor that continuation turns the converter off and moves the
clock to the file, as before. Only audio that has to be converted is
converted.

**The counting.** `frames_out` still counts file frames, because the
seek anchor, the cue cut, the index table and the length are all in the
file's frames. What goes to `s_frames_out[]` and `s_frames_rate[]` is in
ring frames (`ring_frames_of()`), because the writer subtracts queued
ring frames from one and divides by the other. A converted block is
credited to `frames_out` in proportion to how much of it the ring has
taken. A seek resets the converter's history.

**Fallbacks.** If the library refuses the pair (it takes multiples of
4000 and 11025) or cannot allocate, `rateconv_begin()` returns false and
the boundary gets the dip, as before. A rate change inside one file
turns the converter off.

**Not this patch.** A USB device that offers only 48 kHz still gets no
audio from a 44.1 kHz file (`audio_out.c`, "can take the format"). That
would be a conversion on the USB route, with its own questions.
Converting to the running rate also leaves the filter's last millisecond
or so of the outgoing track inside the converter at a boundary. That
audio is under a crossfade and is never heard.

### 5022 -- never convert down to a narrow output

On the board, 5021 crossfaded out of `14 wav-mono-22k` into
`15 aac-adts-cbr` and kept the clock at 22050 Hz: `on: 44100 -> 22050
Hz`. That low-passes a whole 44.1 kHz track at 11 kHz to save one
boundary. It did what 5021 said it would, but that rule was wrong for
this case.

The carry now refuses to convert DOWN to an output below 44.1 kHz. That
boundary gets the dip, and the clock moves to the file, as it always
did. Converting down to 44.1 or 48 kHz, or up from anything, is
unchanged. Going from 48 to 44.1 loses nothing anyone can hear.

The same session found the "horribly loud" seek. The converter was
tested on the host first: the P4 objects of `esp_audio_effects` 1.3,
linked into a freestanding harness under `qemu-riscv32`. The rate
converter uses no custom instructions. It has unity gain at 1 kHz in
44.1->22.05, 48->44.1, 44.1->48 and 22.05->44.1. A reset leaves one
block with a 6% overshoot from the phase jump. A full-scale square wave
saturates at the rails without a single wrapped sample. The converter
is not where the level came from.

### 5023 -- USB conversion to a rate the device will take

The Avantree DG80 offers one playback setting: stereo, 16-bit, 48000 Hz.
On Linux, `speaker-test -D hw:` refuses 44100 outright. Under "a USB
device that can take the format wins", it was an output for almost
nothing: every 44.1 kHz file fell back to the jack.

Now a device that cannot take the file's rate is asked which rate it
would take (`uac_nearest_rate()`). The rule is the file's rate if
offered, otherwise the lowest offered rate above it, otherwise the
highest below it. The USB path alone converts to that rate. The I2S
clock still follows the file, so the jack and the speaker stay
bit-exact, and an unplug mid-block falls back to them unconverted. A
device that offers the file's rate still gets the samples untouched.

**Opened on one task, run on another.** `esp_ae_rate_cvt_open()` peaks
at 996 bytes of stack. That was measured under `qemu-riscv32` with the
P4 objects of `esp_audio_effects` 1.3, painting the stack across all
six rate pairs. The writer is a 4 KB task at priority 6 whose headroom
has never been measured. So `audio_out_set_format()`, on the decode
task, prepares the converter, and the writer only processes. A device
plugged in mid-track that needs a conversion is taken at the next
track, and says so (`staying analog until the next track`).
`s_cv_lock` covers the handle and its buffer across the two tasks.

**Order on the USB path.** Convert first, then apply software gain in
place on the converted buffer, then `uac_write()`. That is the same
gain step as before, only on the device's rate. The converter is
separate from 5021's `rateconv`: that one is the decode task's and
carries state between tracks for a crossfade, and this one is the
output's.

When 5021 carries a track at the output's rate and the output is a
48 kHz-only device, audio can be converted twice, file to clock and
clock to device. That only happens on the boundaries 5021 exists for,
and each conversion is transparent at complexity 2.

### 5024 -- a device volume control too narrow to be one

The DG80's feature unit has master volume and mute, so `apply_volume()`
used it. Its range is -15 to 0 dB in sixteen 1 dB steps, and the
driver's 0..100 is spread across that range. The whole slider moved the
level 15 dB, and 0% was still clearly audible. Linux's mixer showed it
parked at -15 dB, and the dongle remembers the setting between hosts.

The span is now probed once per attach: set 0 and read the dB value,
then set 100 and read it again. The driver has no GET_MIN/GET_MAX of its
own. Under 40 dB (`UAC_VOL_MIN_SPAN_DB`), the device is left at the top
of its range and the slider becomes the software gain every
control-less device already gets. That gain reaches silence and moves
like the other outputs. The probe holds the top for one control
transfer at the start of a stream before the real level replaces it.

**Also fixed here.** The "no volume control" latch was a `static` inside
`apply_volume()` and was never reset, so one device without a control
put every device attached after it on software gain until a reboot.
It is per attach now, like the probe, and both are cleared in
`handle_connect()`.

### 5025 -- usb_host_uac vendored, as released

`components/usb_host_uac` is `espressif/usb_host_uac` 1.5.0, the version
`dependencies.lock` pinned. It was taken from esp-usb at `6d24137`, the
commit that released it, with its tests and examples left out. It
replaces the registry dependency in `main/idf_component.yml`. The
directory name matches the component name `main` REQUIRES, so nothing
else changes.

This patch vendors the driver and nothing more, so 5026's fix to it
reads as a diff against the released code rather than being buried in
thousands of new lines. One edit was needed to make it build here: the
`usb` dependency in its manifest applies only from IDF 6.0 and points at
a path inside the esp-usb repository. It is dropped, and a comment says
why. On IDF 5.5, the component's own CMakeLists.txt already requires
IDF's built-in `usb`.

### 5026 -- the DG80's endpoint descriptors, in the DG80's order

The first board run with the DG80 on 5023/5024 never streamed. It
failed on the first start:

    W tab5_uac: device offers no 44100 Hz 2 ch 16-bit setting
    E uac-host: uac_host_device_start(2407): Calculated packet size
                exceeds endpoint max packet size
    E tab5_uac: stream start failed (ESP_ERR_INVALID_SIZE)

48 kHz stereo 16-bit is 192 bytes a millisecond, and the DG80's
endpoint allows exactly 192 (`wMaxPacketSize 0x00c0`). The check could
only fail if the driver thought the maximum was smaller, and it thought
it was 0.

`uac_host_interface_add()` reads each alternate setting's descriptors in
order and stopped at the class-specific endpoint descriptor ("we has got
enough information"). That assumes the standard endpoint descriptor came
first, which is the order the UAC 1.0 spec gives. The DG80 lists them
the other way round. `lsusb -v` shows its AudioStreaming Endpoint
Descriptor among the interface's descriptors, ahead of the Endpoint
Descriptor, rather than after it. So the parse stopped before
`ep_mps`, `ep_addr` and `interval` were ever set, and nothing could
stream to it at any rate. 5023's conversion was never reached. Current
esp-usb master still stops at the class-specific descriptor.

Now the parse stops once it has seen both descriptors, in either order,
or at the next interface descriptor. The second stop keeps a device
that lacks one of them from having the next alternate's endpoint read
into this one. On a device that follows the spec, the loop ends exactly
where it did before.

Worth sending upstream to esp-usb. When a release carries it, 5025 and
this patch can both go, and `main/idf_component.yml` gets its registry
line back.

### 5027 -- the Wi-Fi netifs go before esp_wifi_deinit()

Switching Wi-Fi off while a stream was retrying panicked:

    esp_wifi_remote: esp_wifi_internal_reg_rxcb: sta: 0x0
    Guru Meditation Error: Core 0 panic'ed (Instruction access fault)
    MEPC 0x00000000, RA esp_pbuf_free
      pbuf_free <- tcp_seg_free <- tcp_free_ooseq <- tcp_pcb_purge
      <- tcp_abort <- tcp_netif_ip_addr_changed <- netif_remove
      <- esp_netif_destroy_default_wifi

`wifi_stop()` called `esp_wifi_deinit()` and then destroyed the netifs,
which is the order the IDF examples use. Destroying a netif aborts the
TCP pcbs bound to its address and frees what they still hold. Freeing a
pbuf the Wi-Fi driver handed up calls back into the driver to release
its rx buffer. With the native driver that function always exists. With
`esp_wifi_remote`, `esp_wifi_deinit()` unregisters it. The retries had
left sockets with out-of-order segments still queued, and their free
jumped through the NULL.

The netifs are now destroyed between `esp_wifi_stop()` and
`esp_wifi_deinit()`, while esp-hosted and the free path are both still
up. The start-failure path in `wifi_start()` keeps its order, because it
fails before the radio has connected, so nothing can be holding a
received pbuf.

The same log had `eh_host_feat_rpc: request: no response ... (5000 ms)`
and a run of `eh_sdio: mempool OOM` just before the stop, plus `major
version mismatch -- OTA coprocessor from host` (host 3.0.8, coprocessor
reporting 0.0.0) at start. Those are the coprocessor struggling, not
this bug. The 5-second RPC timeout only made the stop slower to reach
the crash.

### 5028 -- a ping before the lookup

`net_online()` means an interface holds an address. It does not mean
packets are moving. The 5027 log had Wi-Fi "connected" through a
coprocessor that had stopped passing traffic (`eh_sdio: mempool OOM`,
RPCs timing out). Each stream attempt then spent 5 s in `select()` or
14 s in `getaddrinfo()` before failing, and each failure counted, so the
backoff schedule ran out against a fault that had nothing to do with
the station.

`net_probe()` in `ethernet.c` sends one ICMP echo to the default route's
gateway, and one to its DNS server when that is a different address. It
uses `esp_ping` (a session per echo, its own task, a semaphore on the end
callback). Before every attempt, netstream now requires `net_online()`
and a gateway that answers. A gateway that stays silent is handled like
"no network": waited out, not counted, re-probed every 2 s
(`NET_PROBE_EVERY_MS`), and bounded by the same 25 s `NET_WAIT_MAX_MS`,
after which the attempt goes ahead and fails honestly.

Only the gateway gates. The DNS server's answer is logged, not required:
plenty of public resolvers drop ICMP, and a lookup that then fails
still puts its own error in the log. The line reads like `gateway
192.168.5.1 3 ms, dns 8.8.8.8 14 ms`. "Gateway answers, DNS server does
not" and "nothing answers" are different faults, and before this they
looked the same.

Cost on a working network: one LAN round trip, single-digit
milliseconds, plus a DNS-server echo when the resolver is off-LAN. The
ping task's stack is taken from the heap for each session.

### 5029 -- the internet, not just the gateway

5028's second echo went to the route's DNS server, and only when that
was not the gateway. On most home networks it is the gateway, so the
LAN was all that got checked. Now, once the gateway answers, the probe
pings 8.8.8.8, and 1.1.1.1 if 8.8.8.8 is silent. That proves the uplink
before a lookup is spent finding out: `gateway 192.168.5.1 3 ms,
internet 8.8.8.8 14 ms`. "LAN up, no internet" (a dead modem, a captive
portal) now reads as that, not as a DNS failure.

The gateway still decides and the internet echo is only reported. Some
networks drop outbound ICMP entirely, and gating on it would make every
station there wait out the full `NET_WAIT_MAX_MS` before trying. The
second anchor is there so one resolver's ICMP policy is not mistaken for
no internet. On a working network the cost is one extra WAN round trip
per attempt.

### 5030 -- the heap when a USB device arrives, and when a stream dies

Two board runs, the same shape. A radio stream plays over Wi-Fi, the
DG80 is plugged in, and within two seconds the stream's read fails and
the link stays dead. The second run shows why the link is dead:

    33306  USBH_CDC: New device connected, address: 1
    33314  tab5_uac: USB audio output attached (itf 2, addr 1)
    35098  tab5_netstream: read failed after 492565 audio bytes
    39104  eh_sdio: dma_alloc(5120) failed; dropping read
    45117  tab5_netstream: gateway 192.168.5.1 no answer; waiting ...

`eh_sdio` could not get a DMA-capable buffer for the coprocessor's
rx path, so Wi-Fi traffic stopped. 5028's probe reported that as a
silent gateway rather than a 14 s lookup. The boot log reserves 32 KB of
internal memory for DMA and internal allocations. The SDIO transport
and the USB host both draw on it, and a new USB device adds to that
host's side: control transfers, the HID interrupt transfer, the CDC
probe that `iot_usbh_cdc` runs on every device, and the UAC interface.
That is a hypothesis, not a finding. This patch is here to test it.

`uac.c` logs internal and DMA-capable free memory, with the largest
block of each, just before and just after `uac_host_device_open()`.
Netstream logs the same four numbers the moment a read fails. The
largest block matters because the failing allocation is 5120 bytes, and
fragmentation can refuse that with plenty free in total.

If the numbers confirm it, the fix is to give the DMA pool more room or
make the USB side take less, and the numbers will say which.

### 5031 -- the supply when a USB device arrives, and when it streams

The other half of 5030's question. Memory is one way the DG80 arriving
could take Wi-Fi down. The supply is another. battery.h already records
a bus-powered keyboard browning the board out on every plug-in, so a
load step on the USB-A port is known to be able to reach the SoC. The
Wi-Fi coprocessor sits on the same supply.

`battery_trace_arm()` now fires at two moments. The first is
`enum_filter()`, which runs as each device is about to be configured,
the point where it is allowed its full current. The second is a
successful `uac_stream_start()`, where isochronous data starts flowing
and, on a Bluetooth transmitter, its radio starts sending. Each trace is
the existing 1200-sample window with minimum, mean and sag, already
used for seeks.

What the trace sees is the pack, through the INA226. A dip on the 5 V
or 3.3 V rail downstream of the regulators will show there only as the
current step that caused it, so a small sag here does not clear the
rails. A large one convicts them.

### 5032 -- the UAC ring in PSRAM

5030 and 5031 answered the question they were written for:

    trace (usb device): ... min 8060 mV ... sag 27 mV
    heap before open: internal 57103 free (largest 31744), DMA 18139 free (largest 10240)
    heap after open:  internal 36591 free (largest 15360), DMA 13419 free (largest 7424)
    read failed after 482534 audio bytes
    eh_sdio: dma_alloc(8192) failed; dropping read

It is not the supply. The pack moved 27 mV. It is memory. Opening the
DG80's output interface took 20.5 KB of internal RAM before a single
sample had been streamed, and it left the largest DMA-capable block at
7424 bytes. The Wi-Fi coprocessor's SDIO transport then asked for 8192,
did not get it, and dropped reads until the link was dead.

Most of the 20.5 KB is `xRingbufferCreate(16 KB)` in
`uac_host_interface_add`, which takes its storage from the internal
heap regardless of size. The ring is only touched from tasks: the
writer through `uac_write()`, and the driver's transfer callbacks, which
run on its client task. It can live in PSRAM. The vendored driver now
creates it with `xRingbufferCreateWithCaps(..., MALLOC_CAP_SPIRAM)`,
falls back to the old call if that fails, and deletes it with whichever
call matches (`xRingbufferGetStaticBuffer()` tells a WithCaps ring from
a plain one). The ~4.7 KB of DMA the open also took is transfers and
descriptors, and stays.

5030's heap lines stay in, so the next board log shows what the open
costs now. If the DMA pool is still short, the next step is the
coprocessor's buffers, not the USB side: esp-hosted allocates each rx
buffer as it goes rather than from a pool it holds.

### 5033 -- ESP-Hosted SDIO receive into fixed buffers

5032 moved the UAC ring to PSRAM, and the board showed it worked
(internal cost of the open: 20.5 KB down to 4 KB). The stream still
died a second after the DG80 arrived. The DMA-capable side was nearly
unchanged: 17191 free, largest 10240, down to 13187, largest 7168.
After that, pings answered (`gateway ... 7 ms, internet 8.8.8.8 45 ms`)
and every TLS connection timed out. Small packets got through and large
reads did not.

That is how ESP-Hosted's default receive mode behaves.
`CONFIG_ESP_HOSTED_HOST_SDIO_RX_STREAMING_MODE` reads each burst from
the C6 into one of two DMA buffers that grow on demand
(`sdio_rx_get_buffer()`: a larger read frees the buffer and allocates a
bigger one, contiguous). The transport therefore needs an 8-9 KB
contiguous DMA block whenever traffic gets bursty. The note in
sdkconfig.defaults about 0044's TCP window is the same failure under
load. A USB device's transfers are enough to fragment the pool below
that size.

`CONFIG_ESP_HOSTED_HOST_SDIO_RX_MAX_SIZE` reads each transfer into a
fixed one-packet buffer (`MAX_SDIO_BUFFER_SIZE`, about 1.6 KB), which a
fragmented heap can still supply. It costs throughput, one SDIO transfer
per packet rather than per burst, and nothing here streams fast enough
to notice.

It is a Kconfig choice, so it needs `rm sdkconfig`. The C6 reports
firmware 0.0.0 and no SW_AGGR, and whether it is happy with a host
reading fixed sizes is the board's to answer. If Wi-Fi does not come up,
this is the line to take back.

### 5034 -- usb_host_msc vendored, as released

`components/usb_host_msc` is `espressif/usb_host_msc` 1.3.0, the version
`dependencies.lock` pinned, taken from esp-usb at `d96b561` without tests
or examples. It replaces the registry line in `main/idf_component.yml`.
The one edit is the one 5025 made to the UAC driver: the manifest's
IDF-6-only `usb` dependency is dropped. The pre-6.0 build path
(`diskio_usb.c`, no BDL) is what IDF 5.5 compiles. This patch is
vendoring only; 5035 is the fix, readable as a diff against released
code.

### 5035 -- a failed MSC transfer alloc, and 16 KB commands

A USB stick plugged in during a Wi-Fi stream. The stream survived it,
which answered 5033's question: the DG80's effect on Wi-Fi is that
device's, not USB's in general. But the library reindex that followed
panicked the heap:

    E USB_MSC: msc_bulk_transfer(689)
    assert failed: tlsf_free tlsf.c:630 ("block already marked as free")
      urb_free <- usb_host_transfer_free <- msc_bulk_transfer (msc_host.c:688)
      <- bot_execute_command <- scsi_cmd_sense <- scsi_cmd_read10
      <- usb_disk_read <- f_read (16384) <- ... <- covertag <- medialib

**The double free.** `msc_bulk_transfer()` grows its transfer by freeing
the old one (line 688) and then allocating the larger one (689). When
689 fails, it returns with `device->xfer` still pointing at the freed
transfer. The failed read is followed by a REQUEST SENSE, whose transfer
call reads that pointer and frees it again. The new transfer is now
allocated first, and the old one freed only once the new one exists. A
failed allocation leaves the old transfer in place and the command
fails cleanly. esp-usb master has the same two lines.

**Why the allocation failed.** FatFs hands a whole multi-sector read to
the disk, and the player reads in 16 KB chunks. So the transfer needed a
16 KB contiguous DMA-capable buffer, and with Wi-Fi up the largest such
block is about 8 KB (5030's numbers). `usb_disk_read()` and
`usb_disk_write()` now issue at most 4 KB per SCSI command
(`USB_DISK_MAX_XFER_BYTES`). That is more round trips, which a USB 2.0
stick does not notice at audio bitrates. It also means the transfer
never grows past 4 KB, so the path above is not reached in the first
place.

### 5036 -- streaming SDIO receive back

5033 selected `ESP_HOSTED_HOST_SDIO_RX_MAX_SIZE`. Once it actually
reached a build (it needed `rm sdkconfig`, and the first board run after
it was still on the old choice), Wi-Fi did not work with nothing on USB
at all. The boot said `SDIO Host operating in PACKET MODE`, the first
probe answered (`gateway 2 ms, internet 50 ms`), every TLS connect timed
out at 5 s, and then the gateway stopped answering. That happened on two
stations and again after a Wi-Fi off/on.

The coprocessor reports firmware 0.0.0 and `CP without SDIO SW_AGGR;
compatible streaming mode enabled`. It frames its reads for a streaming
host, and a host reading fixed packet-sized buffers loses them. 5033's
own note named this as the first suspect.

`sdkconfig.defaults` now says `STREAMING_MODE=y` explicitly, so a
regenerated sdkconfig gets it whatever the component's default becomes.
The DMA-fragmentation problem 5033 was aimed at is still open. The
remaining levers are on the allocation side, not the transport mode:
how much ordinary `malloc()` lands in internal RAM
(`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`), and updating the C6's firmware
to match the host so the newer transport options exist at all.

The same run confirmed 5027: Wi-Fi off and on again with a stream
retrying, and no panic.

### 5037 -- the conversion's cost, measured, and a valve

With Wi-Fi off, the DG80 in the port and a 44.1 kHz file, the whole
USB chain came up for the first time:

    uac-host: Set EP 3 frequency 48000
    tab5_uac: streaming: alt 1, 2 ch, 16-bit, 48000 Hz               (5026)
    tab5_audio: USB device takes 48000 Hz; converting 44100 -> 48000 Hz (5023)
    tab5_uac: device volume spans only 15 dB ...; held at its top   (5024)
    tab5_audio: output: USB audio

Then the task watchdog fired twice. IDLE0 was starved, `i2s_wr` was the
running task, and the second dump was inside `fa_resample_process`. The
decode task got only the gaps (`ring send blocked 500 ms` with the ring
empty, every half second).

Under qemu the P4 objects of `esp_audio_effects` 1.3 cost about 37 M
instructions per second of 44.1->48 kHz stereo at complexity 2, in either
perf variant. That is a tenth of a 360 MHz core. The hardware disagreed
by a large factor. Memory is the suspect. The output buffer was in PSRAM,
the library was in its MEMORY variant, and on this board PSRAM also
feeds a 720x1280 display that already logs DSI underruns. qemu has no
cache and no contention, so its instruction count cannot see that.

So:

- The library runs in its SPEED variant (tables in internal RAM), and
  the output buffer is allocated internal first, falling back to PSRAM.
- Each slice's `esp_ae_rate_cvt_process()` is timed. Once per second of
  input the log says `USB conversion 44100 -> 48000 Hz: N% of real time,
  worst slice M us`.
- Past `CONV_MAX_LOAD_PCT` (50%) over a second, the USB route is
  abandoned for the rest of the track (`s_cv_too_slow`, which
  `arbitrate()` now respects) and the audio goes to the jack. The next
  track tries again. The writer at priority 6 cannot be the reason the
  board resets, whatever the cause turns out to be.

### 5038 -- the converter's inner loop in IRAM

5037's valve caught the overload and gave the measurement:

    USB conversion 44100 -> 48000 Hz: 234% of real time, worst slice 56024 us
    USB conversion too slow (234% > 50%); analog for the rest of this track

That is with the SPEED variant and an internal output buffer, so the
memory hypothesis was wrong. qemu counts about 37 M instructions per
second of audio for the same objects, which is a tenth of a 360 MHz
core. The board is spending twenty-odd times that.

Instruction count cannot see the instruction fetch. The library is
precompiled into flash and executes in place, and this board's flash
runs at 40 MHz DIO (the boot log's `SPI Speed: 40MHz`, `SPI Mode: DIO`).
The cache that serves it is shared with the decoder, the display code
and everything else. An inner loop evicted between slices refetches over
that bus.

`main/linker.lf` maps `rsp_proc` from `libesp_audio_effects.a`
(`noflash`) into internal RAM. That object holds `fa_resample_process`,
`fa_interp_process` and `fa_decimate_process`, the per-sample paths,
about 5.3 KB. A full link in the host harness puts them at 0x4ff0....
The 64-bit divide they call, `__divdi3`, already resolves to ROM
(0x4fc0....). Open and close stay in flash, since they run once per
track.

5037's measurement stays, and its log line says whether this was it. If
the load is still over the valve's 50% with the loop in IRAM, the
flash is exonerated. The remaining knob would then be the converter's
complexity (1 is about half the instructions of 2), and past that the
flash mode itself (QIO/80 MHz), which is a board question, not a
code one.

### 5039 -- The USB path's own resampler

5038 put esp_ae_rate_cvt's inner loop in IRAM and the board still said
`223% of real time, worst slice 53120 us`. Espressif's own table for
that library (docs/README_RATE_CVT.md, ESP32-S3 at 240 MHz) gives
44.1 -> 48 kHz at complexity 2 as 1.7% of a core. The P4 build of 1.3.0
is two orders of magnitude off its own documentation, on a pinned
version this tree cannot move past (1.4 needs rev 3 silicon), and
nothing left to try from outside a precompiled archive. Its per-sample
object calls a 64-bit divide (`__divdi3`) from four places, which is
the likeliest story and not one this tree can fix.

So the USB path stops using it. `main/polyrsp.c` is a plain polyphase
resampler: the reduced ratio L/M (160/147 for 44.1 -> 48 kHz), one
Kaiser-windowed sinc (beta 6) designed in floating point at open, split
into L rows of 48 Q15 taps (scaled up by in/out when converting down,
at most 96), each row normalised to unity gain. Per output frame it is
48 multiply-adds per channel into int32 and a saturating shift, no
divides; the phase step is an add and a compare. 44.1 -> 48 kHz is
about 4.6 M multiply-adds a second. The table is 15 KB at that ratio,
in PSRAM so the radio's DMA keeps the internal RAM; the row a sample
reads is 96 contiguous bytes.

Measured on the host against a fitted sine, stereo, 1024-frame blocks
and odd block sizes alike: 72-80 dB SNR through 19 kHz for 44.1 -> 48,
48 -> 44.1, 88.2/96 -> 48 and 22.05/32/11.025 -> 48; passband within
0.15 dB at 19 kHz; content above the lower rate's Nyquist gone. That is
below what esp_ae claims at complexity 2 and far above what reaches a
car through a Bluetooth codec.

5037's log line and valve stay unchanged, so the board's number decides
it. rateconv.c (the crossfade carry, 5021) still uses esp_ae on the
decode task; if this one measures well, that is the next thing to move.
5038's linker fragment now places code nothing on the USB path calls;
left in until that is settled.

### 5040 -- The crossfade's converter onto polyrsp

5039 measured 9% of real time for 44.1 -> 48 kHz on the board where
esp_ae_rate_cvt measured 223%. rateconv.c, which converts an incoming
track to the running output rate so a crossfade can span a rate change
(5021), was still on esp_ae -- on the decode task, where the cost hid
as a slower decode rather than a watchdog, but the same cost.

rateconv.c now wraps polyrsp. The interface is unchanged: begin with
keep for the gapless case, reset for a seek (polyrsp_reset(), new,
zeroes the history), run on any block size. polyrsp takes a bounded
block, so rateconv_run() feeds it RATECONV_SLICE (4096) frames at a
time into one output buffer sized for the whole block plus a frame per
slice.

Nothing calls esp_audio_effects now. The dependency, its lock entry and
5038's linker fragment stay for this patch: dropping the dependency
re-solves dependencies.lock, and the last re-solve is what pulled in an
esp_audio_codec this silicon cannot run. The fragment places code the
linker's section GC then discards, so it costs nothing. Both go in a
patch of their own, with the lock checked by hand.

### 5041 -- One chooser listing at a time

At boot with a track to resume, the chooser came up showing the card's
top-level folders and the resumed album's files in one list, under the
album's path. Tapping a top-level folder then failed:

    button: row 1 (dir) "test_audio_files"
    cannot open /sd/Selections_from_the_2005-2006_Season-12519/test_audio_files

It was two load_dir() calls running at once. The decode task's
restore_last_track() reopens the chooser on the track's folder, and
ui_task's browser_draw() sees the card's mount generation change with
an empty list and loads the tab's root. Each empties the list, sets
s_dir and appends. Interleaved, they produced one list with one path.

load_dir() now holds a mutex (created statically on first use), and the
two draw-side loads re-check their condition under it. A draw that
queued behind browser_open() therefore finds the folder already listed
and leaves it, rather than replacing it with the root. The draw loop
still reads the rows without the lock, as before; a load re-dirties the
screen when it finishes, which is what already covered that.

The same boot also played the 06 -> 07 crossfade at 44.1 kHz across
Opus's 48 kHz on 5040's converter ("on: 48000 -> 44100 Hz", "crossfade
done; dropped 0 KB"). It sounded right.

### 5042 -- Bitmap consumer keys, from the descriptor

The first run with the DG80 in the car, logged from a phone, had every
key from its remote interface arriving and none acted on:

    tab5_hid: remote: 01 01 00 -> Vol+
    tab5_hid: itf 3: consumer usage 0x0001 unmapped
    tab5_hid: remote: 01 00 00 -> Vol+

The interface declares the Consumer page and Report IDs, so it is
classified HID_KIND_CONSUMER. report_consumer() then reads the two bytes
after the ID as a usage code, which is right for an array field and
wrong for this one. The DG80 sends Report 1 with sixteen one-bit fields,
and bit 0 set reads as "usage 0x0001". The "-> Vol+" on the raw line is
also wrong: it is the headset remote's bit table applied to the Report
ID byte.

report_desc_scan() now also walks the descriptor for its fields (Usage
Page, Report Size/Count/ID, Usage and Usage Min/Max, Input items).
Each Variable, non-Constant, one-bit Input on the Consumer page maps its
bits to its usages in order, per report ID. report_consumer() dispatches
a bit on its rising edge against the previous report of the same ID,
and falls back to the array reading for any report with no bitmap
fields. The raw line keeps the bit names only for the BITMASK kind.

The descriptor is logged as hex at attach, followed by one line per
mapped bit, so the next log says what bit 0 of the DG80 is rather than
this entry guessing. Tested on the host against a CSR-style sample
descriptor; the DG80's own (98 bytes, "UNAVAILABLE" to lsusb without
root) has not been seen yet.

The same log has the USB conversion at 7-8% in the car, and the phone
as the serial monitor works: Serial USB Terminal on Android, the Tab5's
USB-C port.

### 5044 -- Where the DG80 stands (board notes, no code)

Logged from the car and the bench on builds through 5042, with the
phone as the serial monitor (Serial USB Terminal on Android, on the
Tab5's USB-C port).

WORKING
- Enumeration, 48 kHz streaming, and 44.1 -> 48 kHz conversion on the
  USB path at 7-9% of real time, worst slice about 4.5 ms (5039).
- Crossfade across a rate change on polyrsp (5040): 06 Vorbis 44.1 ->
  07 Opus 48, "crossfade done; dropped 0 KB".
- The 15 dB device volume detected, held at the top, gain in software.
- Car play/pause over AVRCP -> DG80 -> HID report 1 bit 0 (0x00CD) ->
  HID_BTN_PLAY_PAUSE (5042). The Prius sends the same toggle for both
  its play and pause buttons. Pausing on the Tab5 and then pressing
  play in the car stays in step.
- USB power off and on from the settings panel mid-track: the DG80
  re-enumerates about 0.4 s after VBUS returns and takes the route back
  at once, without waiting for the next track.

THE DESCRIPTOR, as the board read it (98 bytes, itf 3):

    05 0C 09 01 A1 01 85 01 15 00 25 01 09 CD 09 B5 09 B6 09 B7 75 01 95 04 81 02
    15 00 25 01 09 B0 09 B1 09 B3 09 B4 75 01 95 04 81 22
    15 00 25 01 09 E9 09 EA 09 E2 75 01 95 03 81 22 75 05 95 01 81 01 C0
    06 A0 FF 09 01 A1 01 85 02 09 01 15 00 26 FF 00 75 08 95 12 91 00
    09 02 75 08 95 12 81 00 C0

Report 1, Consumer: bits 0-3 Play/Pause, Next, Previous, Stop; 4-7
Play, Pause, Fast Forward, Rewind; 8-10 Volume Up, Volume Down, Mute;
five bits of padding. Bits 4-7 have no entry in CONSUMER_USAGES yet and
log "unmapped" if they arrive. Nothing so far says the Prius sends
anything but bit 0.

Report 2, vendor page 0xFFA0: an 18-byte Output report and an 18-byte
Input report. A private command channel -- Avantree's configuration
tool, most likely. Undocumented; nothing here writes to it.

NOT EXPLAINED
- One drive ended with the DG80 absent and a panel power cycle not
  bringing it back; a reboot of the Tab5 did. The log excerpt started
  after it had already dropped, so the cause is unknown. The bench power
  cycle above works, so it was not that path.

LIMITS
- No track titles on the car's screen. USB audio has no metadata
  channel, and the DG80 has nothing to give the car over AVRCP. The
  Prius shows a generic source.
- The DG80 must be plugged directly into the Tab5; the host stack cannot
  reach a full-speed device behind a high-speed hub.
- Wi-Fi with the DG80 attached is still short of DMA-capable internal
  RAM (5032/5036). Unchanged.

### 5045 -- Both Ethernet routes confirmed on the board

v0.4.0's summary said the Realtek path "has not been on hardware yet".
It has now, on an RTL8152 (0bda:8152), in builds g bf7b6f2 and g9a80628:

    tab5_usbhost: 0bda:8152: asking for configuration 2 (CDC-ECM)
    iot_usbh_ecm: ECM interface found: VID: 0BDA, PID: 8152, IFNUM: 0
    tab5_eth: ecm: address 192.168.1.125, gateway 192.168.1.254
    tab5_eth: ecm: wired network up; it is the default route
    tab5_netstream: hop 1: HTTP 302 -> redirect, ... via cable (ecm) 192.168.1.125
    tab5_eth: ecm: cable disconnected
    tab5_eth: ecm: wired network down

So ethcfg's configuration-2 selection, DHCP, taking the default route,
an HTTPS radio stream with ICY metadata over the cable, and a clean
unplug all work. The ASIX AX88772 did the same in the same session
(192.168.1.124), as it had at v0.4.0.

The RTL8153 (gigabit) takes the same code path and has not itself been
plugged in. README and the esp_usbh_asix pin's comment in
idf_component.yml are updated to match; the pin's "has not yet run on
hardware" was the driver's own claim and was out of date here.

### 5047 -- esp_audio_effects removed

Nothing has called it since 5040: the USB path (5039) and the crossfade
carry (5040) both use polyrsp.c. This drops the dependency from
main/idf_component.yml. It also drops main/linker.lf and its
LDFRAGMENTS line, 5038's IRAM mapping for the library's rsp_proc object,
which placed code nothing reached.

The reasons it went, for whoever reaches for it again: its P4 build of
1.3.0 measured 223% of real time for 44.1 -> 48 kHz stereo on this
board, flash or IRAM alike. 1.4.0 added P4 vector assembly for the
filter, said to be 4x faster (about 56% here, still over 5037's valve),
and 1.4.2 refuses to build for silicon below revision 3.0. 1.4.3 is the
same converter. polyrsp does the same job at 7-9%.

The manifest changed, so the next build re-resolves dependencies.lock
(see CLAUDE.md). Expected: the espressif/esp_audio_effects and
espressif/gmf_fft entries go, esp_audio_effects leaves
direct_dependencies, and manifest_hash changes. Nothing else should
move.

### 5048 -- Two stations kept in flash, for no media

The README's "zero storage internet radio scenario": a Tab5 with a saved
network and nothing mounted had nothing to play, because every station
list lives on a volume. radiokeep.c keeps two in NVS, namespace
"radiokeep", beside the saved networks:

- LAST, the last station to reach first sound. Written with media or
  without, from play_stream()'s first-sound branch when the station is
  not a reconnect of the same one, and only when the name or URL
  changed.
- STAR, one starred station. With any volume present it is neither
  read nor changed: favorites_contains() and favorites_toggle() mean
  favorites.m3u exactly as before. With no volume, those two go to the
  flash slot instead, so the star button keeps working with nothing
  mounted and holds one station.

With no volume present, stations_load() installs STAR then LAST (when
it is a different URL) through stations_set_remote(), labelled "Kept on
this Tab5". A volume present without a stations.m3u keeps its old
answer, no list. The player's generation-gated load now runs with no
volume too, which is what puts the kept list up at a card-less boot and
after a card is pulled.

Each slot is a blob "name\0url\0uuid\0", usually around a hundred
bytes. NVS appends entries and erases a 4 KB page only once it is full,
and LAST is not rewritten for the same station, so the 20 KB partition
takes millions of station changes before its sectors wear. The write
happens on the decode loop at first sound, with seconds already in the
ring, and it is skipped on reconnects.

Built, not yet run on the board. The test: play a station with a card
in, pull the card, reboot, and the RADIO tab should offer it. Star
something with no media, reboot, and it should be first.

### 5049 -- Wi-Fi with no card: the switch in NVS, and the shared SDMMC host

The first card-less boot on 5048 found two things.

THE SWITCH WAS ON THE CARD. The saved networks are in NVS
(wifistore.c, "3 saved networks" at boot with nothing mounted), but the
Wi-Fi on/off setting was only in .defeatist.dat. No card meant the
default, off, so a Tab5 with networks saved and stations kept in flash
had its radio down until the panel switch was used. The switch is now
mirrored in NVS (namespace "radiokeep", key "wifi_on", one byte, written
only when it differs). settings_init() starts from it, a card's record
still overrides it (and is written through), and the panel's toggle
writes through.

TURNING IT ON PANICKED.

    assert failed: xQueueSemaphoreTake queue.c:1709 (( pxQueue ))
    #4 sdmmc_host_do_transaction (slot=1, ...)
    #7 sdmmc_io_reset
    #9 eh_host_port_sdio_card_init

The C6 radio is slot 1 of the same SDMMC controller the card uses as
slot 0. With no card, storage's poll tries to mount every second, and
on failure called sdmmc_host_deinit(), the whole controller. That
deleted the host's transaction queue under esp_hosted's card init. With
a card in, the mount succeeds and never reaches that line, which is why
no earlier run saw it. It is now sdmmc_host_deinit_slot(SDMMC_HOST_SLOT_0).
The mount's own cleanup already deinits slot 0 through
SDMMC_HOST_DEFAULT()'s deinit_p, so the second call returns
ESP_ERR_INVALID_STATE without touching the slot count.

Built, not yet run on the board.

### 5050 -- The shared SDMMC host, again: no second slot release

5049 on the board, no card, Wi-Fi switched on: the same panic, the same
backtrace (xQueueSemaphoreTake from sdmmc_host_do_transaction, slot 1,
inside eh_host_port_sdio_card_init).

5049 swapped storage's sdmmc_host_deinit() for
sdmmc_host_deinit_slot(SDMMC_HOST_SLOT_0) and called it a belt, on the
belief that a second slot release was refused when the mount's cleanup
had already done one. IDF 5.5.5's sdmmc_host_deinit_slot() checks only
that the host is up. It then decrements num_of_init_slots whether or not
that slot was initialised, and deinitialises the whole controller when
the count reaches zero. The failed mount's own cleanup (call_host_deinit
-> deinit_p -> sdmmc_host_deinit_slot(0), since SDMMC_HOST_DEFAULT() sets
SDMMC_HOST_FLAG_DEINIT_ARG) had already taken the count from 2 to 1. The
second call took the radio's 1 to 0.

So storage calls nothing after a failed mount. The cleanup inside
esp_vfs_fat_sdmmc_mount() is the release, and it is the only one. The
GPIO conflict the old sdmmc_host_deinit() was added for came from an
older IDF, whose mount cleanup did not release the slot.

Rule for anything else that touches this controller: exactly one
sdmmc_host_deinit_slot() per successful sdmmc_host_init_slot(), and
never sdmmc_host_deinit().

### 5051 -- settings_init() after NVS, and a word from the kept list

5050 on the board, no card: Wi-Fi switched on from the panel came up
and joined. The shared-host panic is gone. Two things were still wrong.

THE SWITCH WAS NEVER READ. app_main() called settings_init() before
nvs_flash_init(). The NVS read 5049 put in settings_init() therefore
always failed, and a card-less boot always had the radio off. The
comment above the NVS init said the two were unrelated, which was true
until 5049. settings_init() and the volume push now follow
wifistore_init(). Nothing between the two places reads a setting.

THE KEPT LIST CAME UP EMPTY on one card-less boot, after a station had
been kept (`last played kept: Спокойное радио`) and after an earlier
boot had shown `1 stations from Kept on this Tab5`. The log could not
say why. radiokeep_list() now logs, once per load, which slots it found
and the NVS result of the last read:

    tab5_keep: kept list: star no, last yes (last: ESP_OK, 58 bytes)

The same log also showed:
- `E BOD: Brownout detector was triggered` and the INA226 reading about
  1.85 V. The board was on USB power only, with no battery. Not a code
  problem, but a USB-only Tab5 with Wi-Fi and a USB device attached can
  brown out.
- With Wi-Fi up, `eh_sdio: mempool OOM` bursts, a stream read failure,
  and the RTL8152 failing to claim its interface (`EP Alloc error:
  ESP_ERR_NO_MEM`). DMA-capable internal RAM fell to 5 KB free. That is
  the known Wi-Fi + USB DMA shortage (5032/5036), still open.

### 5052 -- TLS without the crypto engines' DMA

The log that ended 5051's run had the RTL8152 streaming when Wi-Fi was
switched on. DMA-capable internal RAM fell to 1395 bytes free, and the
AES engine could not get its descriptors:

    esp-aes: Failed to allocate memory for the array of DMA descriptors
    esp-tls-mbedtls: read error :-0x0001
    esp-tls-mbedtls: mbedtls_ctr_drbg_seed returned -0x0001

The stream dropped, the directory lookup failed, and the C6 never
answered esp_wifi_init. mbedTLS's own buffers were already in PSRAM
(CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC, from the stream-probe work). What was
left was the P4's AES and SHA accelerators, which allocate DMA
descriptors and alignment buffers from internal DMA RAM on every
operation.

sdkconfig.defaults now turns off CONFIG_MBEDTLS_HARDWARE_AES (and with
it HARDWARE_GCM) and CONFIG_MBEDTLS_HARDWARE_SHA. The bignum and ECC
engines stay, since they take no DMA memory. Cost: software AES and SHA
on a 360 MHz core, against 8-40 KB/s of ciphertext for a radio stream.
That is well under 1% of a core by the usual cycles-per-byte figures
(not measured here). Handshakes are already network-bound at 0.7-2.2 s.

One side effect at build time: esp_crt_bundle.h uses bool and had only
been getting <stdbool.h> through the hardware AES headers.
radiobrowser.c now includes it first. netstream.c already had it by way
of its own header.

Needs `rm sdkconfig` before building. The existing sdkconfig keeps
HARDWARE_AES=y otherwise.

### 5053 -- The Wi-Fi switch, acted on with no card

5052 on the board: with the cable streaming and Wi-Fi switched on, the
stream held (no esp-aes failures). The radio came up half-way instead:
`mempool OOM start (TX)` with no end, the first RPC unanswered, scans
refused. Internal free fell from 80 KB to 42 KB. The next boot says `CPU
has been reset by WDT`, so that half-up radio probably ended the
session. The tail that would name the task was not captured.

The same boot showed the other half of 5049 not working: Wi-Fi had been
left on, and nothing brought it up. The radio's only boot push is
wifi_apply_settings() in restore_last_track(), inside the gate that
waits for a card's settings to be adopted. With no card that gate never
opens. The switch was in NVS and read in time (5051), and nothing
applied it.

The idle loop's generation-gated branch, which 5048 already runs with
no volume, now calls wifi_apply_settings() when nothing is mounted. It
is a comparison and returns at once when the radio already matches.

Still open: Wi-Fi does not fit beside USB Ethernet (the radio's own
buffers), and a start that half-fails is not powered back down.

### 5054 -- Small allocations to PSRAM too

5053 on the board: the card-less boot brought Wi-Fi up by itself
(`joined fivescore`, NTP synced), so the switch works. Then the kept
station, a 64 kbit/s AAC stream over Wi-Fi with no USB device attached,
failed within a second of first audio:

    tab5_netdec: AAC (ADTS) decoder open (parser-framed); internal free 60091 -> 45231 (cost 14860)
    eh_sdio: mempool OOM start (TX)
    eh_sdio: dma_alloc(2560) failed; dropping read
    tab5_netstream:   internal 43707 free (largest 31744), DMA 4147 free (largest 1536)

After two attempts the gateway stopped answering, and the link stayed
dead.

"Internal free" counts RETENT_RAM and RTCRAM, which are not DMA-capable,
so 43 KB of it meant 4 KB of what the radio needs. The AAC decoder's
14.8 KB was a plain malloc. CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384
sends every plain allocation up to 16 KB to internal RAM first. It is
now 4096, so the decoder's state and every other mid-sized plain
allocation go to PSRAM. Anything that needs DMA memory asks by
capability and keeps getting it.

Watch for: a decode or UI path that slows measurably with its state in
PSRAM. None is expected at these rates. The first number to check is
the `cost` on the decoder-open line, which should fall to near zero.

rm sdkconfig before building.

### 5055 -- A reconnect spliced, not repeated; station logos the hardware refuses

5054 on the board, Wi-Fi, no card, no USB: DMA-capable memory held at
16-17 KB free through a stream, where it had been at 4 KB. It dipped to
4 KB once, with an artwork fetch, a TLS session and the decoder all at
the same moment. The listener reported repeating audio and no artwork.

REPEATS. BBC World Service
(http://stream.live.vc.bbcmedia.co.uk/bbc_world_service) closed the
connection every 6-11 s, 88-134 KB in each time. A clean close: the
reader returned -1 with no transport error, and DMA memory was fine at
every drop. Each reconnect was answered with the server's burst of
recent audio, which had already been received on the connection that
dropped. So the listener heard it again. The PCM reserve grew from 3 s
to 31 s over two minutes of drops, and that growth was the repeats
piling up.

netstream.c now keeps the last 2 KB of audio sent to the ring (after
ICY demux). On a reconnect in the same generation, new audio is held in
a 256 KB PSRAM buffer until those 2 KB turn up in it. Everything
through them is dropped, and the stream continues from the next byte.
With no match in 256 KB, the hold goes out whole, as before. A new
station or a resume (a new generation) never splices. Tested on the
host: three connections with overlapping bursts and random read sizes
produce output byte-identical to the source.

Why the BBC server closes is not known. The URL is an old one. Log:
`reconnect spliced: the server repeated N bytes; skipped`.

ARTWORK. The station's logo was 145x145, and the hardware decoder
refuses sizes not divisible by 8 ("Picture sizes not divisible by 8 are
not supported"). A refusal was the end of the picture. albumart.c now
falls back to TJpgDec on any hardware decode failure, the same path the
can-not-allocate branch already used. A logo costs almost nothing in
software.

### 5056 -- Reconnect at once, no probe after a drop; the kept station's uuid

5055 on the board: `reconnect spliced: the server repeated 20999 /
36397 / 22398 / 22399 bytes; skipped`. The repeats are gone. Two things
were still wrong.

A GAP AT EACH DROP. The amplifier went idle for four seconds mid-stream.
A drop was followed by 1000 ms of backoff, then the 5028 probe, which
took 2.0 s (an echo each to the gateway and to 8.8.8.8, about a second
apiece), then connect and headers, then the splice's hold. That is
about 6 s against a 3-8 s reserve. After a drop from a stream that was
playing, the reconnect now goes at once and skips the probe, since the
link carried audio a moment ago. A reconnect that then fails has
s_failures > 0, and gets the full backoff and the probe as before.

NO ART FOR A KEPT STATION. The logo is looked up by the directory's
uuid, and 5048 kept a name and a URL only. radiokeep's fill() now takes
the uuid from the current station when its URL matches, and LAST is
rewritten when a uuid arrives for a station that had none. A station
kept before this patch gets its uuid the next time it is played from
the directory, not from the kept list, which has none to give.

Also seen: this BBC stream never gets far ahead. After the first burst
it runs at about 1.17x, and the reserve sits at 7-8 s, not the 25-30 s
most stations build. That is the server's pacing, not the ring.

### 5057 -- A held splice is played, not dropped; where the night ended

5056 on the board. The reconnect after a drop went at once (`link was
carrying audio; no probe`), and four splices matched (48-64 KB
skipped). The amplifier still went idle between drops later on:

    W tab5_netstream: hop 1: open failed after 5006 ms     (one reconnect failed)
    ... reconnect, no "spliced" line ...
    I tab5_audio: amplifier off (idle)
    W read failed after 134391 audio bytes                 (dropped again)

After the failed attempt the gap had outrun the server's burst, so the
tail was never found. The hold grew to 134 KB, the server dropped us,
and the next connection's splice_begin() reset the hold. Everything
held was thrown away, every time, and the listener got silence between
drops. Two changes:
- splice_flush() at the end of pump(): a connection that ends while
  holding plays what it held
  (`reconnect ended before the join was found; playing the N bytes held`).
- SPLICE_HOLD 256 -> 96 KB. Bursts seen are 20-64 KB, and a hold that
  has not matched by 96 KB will not.

Host-tested: a clean splice, a reconnect with a gap (held, flushed
whole), then an overlapping reconnect after the flush (spliced), all
byte-exact against the source.

OPEN AT THE END OF THIS SERIES (5039-5057), for whoever picks it up:

- BBC World Service's logo still does not show. The hardware refuses it
  (145x145, not a multiple of 8), and 5055's TJpgDec fallback fails too:
  `esp_jpeg_decode: Error in preparing JPEG image! 8`, JDR_FMT3, an
  unsupported format. Almost certainly a progressive JPEG, which neither
  decoder handles. Needs a progressive-capable decoder, or a directory
  favicon in another format.
- The BBC URL (stream.live.vc.bbcmedia.co.uk) closes every 6-13 s. It
  is now inaudible when reconnects succeed, but the reserve on that
  station never gets past a few seconds, so one slow reconnect is a
  gap.
- DMA memory at first sound on Wi-Fi: artwork fetch + TLS + decoder
  together still dip it to about 6 KB and can cost a drop (`mempool
  OOM`). 5052 and 5054 moved the bulk; this is the peak.
- Wi-Fi does not fit beside USB Ethernet. The radio half-starts (TX
  mempool OOM, first RPC unanswered) and is not powered back down, and
  a later session ended in a WDT reset. Proposed: do not start Wi-Fi
  while the cable holds the route, and power the radio down when its
  first RPC fails.
- Directory lookups just after a Wi-Fi join failed twice (select()
  timeout, getaddrinfo 202) and worked on the third tap. And a tap on a
  directory row while the link is dead logs nothing and shows nothing.
- The DG80 once dropped off USB mid-drive, and only a Tab5 reboot
  brought it back. The cause was not captured. A panel power cycle
  recovers it on the bench.
- DG80 HID report 1, bits 4-7 (Play, Pause, Fast Forward, Rewind) are
  unmapped. The Prius has only sent bit 0 so far.
- One card-less boot showed an empty kept list. It has not recurred
  since 5051 added the line that would explain it.

### 5058 -- A quiet second is not a drop

`pump()` took `esp_http_client_read()` to return 0 for "nothing yet".
In IDF 5.1 through 5.5.5 a read that times out with nothing returns
`-ESP_ERR_HTTP_EAGAIN` (-0x7007), silently at the default log level. With
`SOCKET_TIMEOUT_MS` at 1000, any second without a byte was logged as
`read failed` and reconnected, and the `DROP_SILENCE_MS` path never ran:
`nothing for 5000 ms` appears in no log here. The other way round, a
server's clean close of a body with no Content-Length makes every read
return 0, so a real close waited 5 s before reconnecting.

Now -0x7007 is quiet (the 5 s rule decides), 0 with the body complete
is `server closed the stream ... N ms after the last byte` and
reconnects at once, and anything else is `read failed (n) ... N ms after
the last byte`.

WHAT THE NEXT BBC RUN SAYS. 5055 read its drops as a clean close
returning -1, but the line never printed the value. If the drops stop,
or turn into `nothing for 5000 ms`, they were this. If they go on as
`read failed (-1)` about a second after the last byte, or as `server
closed`, the server is closing them.
