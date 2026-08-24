# M5Tab5 Defeatist Music Player
What do you mean no audio over Bluetooth

## Featues possible and impossible
Claude, do not overwite this section


TODON'Ts:
- No blutooth connectivity of any sort, only aux cable, wired heasphones, or built in speaker
  - LE Audio is not wired to work on the Tab5
  - Traditional bluetooth is not supported by the C6
- No inline microphone controls
  - the signal can be registered ovee the mic line, but there is no chip looking for these signals. And i am not wasting a high priority thread on that.

TODO:

- ~~esp-idf 5.5.5, so m5unified can handle graphics variants~~
- microsd hotplug supported
- exfat format supported for absurdly big files supported
- usb drive hotplug supported (2.0 speeds, be wary of power requirements)
- ~~screen sleep for power (to not turn off chip for touch for wake)~~

TO INVESTIGATE:

- peaking?
- ~~mp3 format variants~~
- why they hell are you telling other people to use a python script. This is an ffmpeg fix if you can't be bothered to pull in libraries

## Claude

Notes on the decode stack, written by Claude. Everything above this
header is yours; nothing in here is load-bearing for the build.

### Why two decoders

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

### What this fixes that the example did not

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

### Things to check before trusting this

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
- Only 16-bit output is handled; `esp_codec_read()` rejects anything else
  rather than playing it as noise. A 24-bit FLAC will refuse to play.

### One-frame-per-call codecs

The header divides esp_audio_codec's decoders into those that "support
input data of any size" and those that "only support input data with a
size of one encoded frame". Only the first group works here, because
`esp_codec_read()` feeds a sliding window from the file rather than
pre-split frames. That excludes `_RAW_OPUS`, `_ALAC`, `_VORBIS`,
`_ADPCM`, `_LC3`, `_SBC` and `_G722`.

`.opus` and `.ogg` therefore both route to `_OGG`, the container parser,
which does take arbitrary lengths. That is also the better mapping in
practice: raw headerless Opus is rare on disk and Ogg-encapsulated Opus
is what everything actually ships.

### Vendored headers are fetched, not committed

minimp3 and pngle are pulled by `cmake/vendored.cmake` during
`idf.py build`, pinned to commit SHAs with a SHA256 per file. Nothing to
run first.

The pin is load-bearing rather than tidiness: `minimp3_prefix.h` lists
every symbol minimp3 exports, and an unpinned upstream that adds one
brings the link collision back for whoever clones next but not for you.
`tools/fetch_vendored.sh` carries the same pins for offline use.

This is the opposite call from `components/fatfs/`, which
`tools/enable_exfat.sh` writes and which is also not committed -- but
that one has no pin, because it is patched from whatever IDF you have
installed.

### On-screen controls

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
the remainder of each is grey. Dragging either raises a ring indicator
offset above the finger, so the thing being adjusted is never under the
hand. Volume applies live during a drag, because you want to hear it. Seek
fires once on release -- re-decoding on every poll would thrash the card.

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

### The title bounces

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

### Touch

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

720x1280 on a 5" panel is about 294 PPI, so an 8 px font glyph is 1.4 mm
tall -- unreadable at arm's length. Everything is scaled for that rather
than left at values that looked right in a 96 PPI mockup: the title is
font8x8 at scale 4 (about 2.8 mm, roughly a phone's body text), album and
artist at scale 3, the MM:SS digits 20x38, the slider thumbs 16 px radius,
and the bar itself 560 px.

Scale 4 fits nineteen characters across the panel, which is why the title
bounces rather than being cut.

If the layout is ever moved again, two things are load-bearing rather than
aesthetic:

- `BUBBLE_ABOVE` has to exceed `SEEK_Y`, or the bubble overlaps the bar.
  That is not cosmetic: the bar is blitted before the bubble is drawn, so
  any part of the bubble inside it gets written to the framebuffer and
  never pushed, and shows stale pixels until the next frame clears them.
- Row 7's centres have to keep the padded hit boxes apart. See above.

### Text rows

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

The font is `font8x8` (public domain), fetched by `cmake/vendored.cmake`
like minimp3 and pngle. It is ASCII 0-127, so `id3_text_to_ascii()`
flattens latin1, UTF-8 and both UTF-16 encodings down to it and turns
anything above 0x7F into `?`. Visibly wrong beats a mojibake glyph that
looks deliberate. Accented artist names will show question marks; that is
the price of not vendoring a Unicode font.

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

This does not make those formats seekable. Ogg seeking is the same
granulepos scan applied as a bisection over page boundaries, which is
worth doing and is not done here.

### The frame walk, for formats with no length at all

`duration.c` covers the containers that state their own length. Raw ADTS,
AMR and a CBR MP3 with no Xing header state nothing -- there is no field
to read, so the only honest answer is to count frames.

`framewalk.c` does one sequential pass reading **only frame headers**.
Every one of these formats puts its own length in its header, so the walk
is header, skip, header, and the audio data is never touched. That makes
it I/O bound rather than CPU bound, which is what makes it cheap enough to
run while a track plays.

The same pass produces the waveform envelope, which is the reason it is
one function and not two. On MP3 the loudness is free: `global_gain` sits
in the side info at a **fixed bit offset**, so it is a constant, not a
parse -- a real per-granule loudness value read without touching a Huffman
table. AAC and AMR headers say how long a frame is but nothing about what
is in it, so those report `has_levels = false` and produce a duration
only; a waveform there needs a real decode, and whether that is worth it
is the caller's call.

Details that matter:

- **It is the backstop, not a replacement for `duration.c`.** The probe
  answers before the first audio block; the walk answers a second or two
  in. Routing FLAC through here would take a bar that is right immediately
  and make it right eventually, for nothing. FLAC also cannot be walked
  cheaply -- the fast path there is SEEKTABLE seeking, which never sees
  most frames and so cannot count them.
- **Resampling takes the max per bucket, not the mean.** A mean over
  twenty frames turns a drum hit into a bump. The transient is the part
  that makes one track look unlike another.
- **The ID3v2 tag is skipped by its length field, not walked past.** This
  was the first version's bug and it is worth stating plainly: the tag
  holds the album art, and 130 KB of PNG is full of bytes that look like
  an MP3 sync word. Resyncing a byte at a time through it locks onto
  noise, parses a nonsense frame length and walks off into the middle of
  the image -- which on the first real file gave 212 frames and a 4 second
  duration for a 52 second track. The size field is syncsafe, seven bits
  per byte with the high bit always clear, so the length can never itself
  contain a false sync.
- **The Xing/Info/VBRI frame is skipped, and not counted.** It is a real
  MP3 frame carrying the seek table rather than audio, and its side info
  is whatever the encoder left there -- an arbitrary value that became the
  spike at the start of every MP3 envelope. Skipped without incrementing
  the frame count, because it is not a frame's worth of playing time
  either.
- **Trailing tags end the walk.** ID3v1 is 128 bytes at EOF, APE tags are
  larger, and the byte-at-a-time resync parsed both into nonsense frames
  whose side info became the spike at the *end* of the envelope. Same
  class of bug as the ID3v2 tag at the front, at the other end of the
  file -- which is the argument for treating "arbitrary bytes adjacent to
  audio" as a category rather than fixing them one at a time.
- **A granule with no main data is silence, and is reported as such.**
  `part2_3_length == 0` means no scalefactors and no Huffman data --
  nothing for a gain to apply to -- and `global_gain` is then whatever the
  encoder left in the field. LAME writes 210 there, and something in the
  run writes 255. Read literally, every LAME-encoded MP3 opens and closes
  at four fifths of full scale.

  That is the spike at each end of the envelope that survived the ID3v2
  and trailing-tag fixes above, and it is not metadata: these are real
  frames, correctly parsed, in the audio stream -- the encoder delay at
  the head and the flush padding at the tail. A 2.6 minute test track has
  51 of them, seven at the front and the rest trailing. They are reported
  as 0 and left out of the normalisation range.
- **The first sync is confirmed by a second one** at exactly the offset
  the first header states, with a matching sample rate. One valid-looking
  header proves nothing; two at the stated spacing do. Once locked, the
  stream is trusted until a header fails to parse.
- **MP3 frames with a CRC shift the side info by two bytes.** The first
  version skipped those frames' loudness instead, which meant a file where
  every frame is protected produced no envelope at all and said only that
  the format had no per-frame loudness.
- **Both buffers are PSRAM and freed.** 32 KB of read buffer and 64 KB of
  per-frame gains, for something that runs once per track.
- **`abort_flag` is polled every 256 frames**, so a track change cancels a
  scan that is no longer wanted rather than finishing it into a buffer
  nobody will read.

Verified against generated files with known frame counts: a 400-frame CBR
MP3 reports 10 s, a 300-frame ADTS stream 6 s, a 500-frame AMR 10 s. Also
against a reconstruction of the file that broke it -- 2000 CRC-protected
64 kbps frames behind a 130 KB ID3v2 tag stuffed with false sync words --
which now reports 2000 frames, 52 s and a full envelope.

Not yet wired to anything -- `decoder_duration_sec()` still returns 0 for
these formats, and nothing draws the envelope. That is the next patch.

### Ogg loudness without decoding anything

Vorbis and Opus have no equivalent of MP3's `global_gain`. There is no
loudness anywhere in the page or packet headers, so an amplitude envelope
means decoding one packet in N -- minutes of CPU on a long track, and a
second codec instance live alongside the one making the sound.

The way in is that **both codecs are always VBR**. A VBR encoder spends
bits where there is something to encode: a silent passage is a handful of
bytes per packet, a dense one several hundred. Packet size is therefore a
usable proxy, and it is free -- the segment table already states it.

What this produces is a **bitrate envelope, not an amplitude envelope**.
Not the same measurement, and worth being honest about: it will not show a
loud sine wave as loud, because a sine wave is cheap to encode. It rises
and falls where the music does, which is all a shape drawn 720 px wide can
convey. The alternative was the format being minutes slower than every
other one.

Packet sizes are clamped rather than scaled adaptively, because a header
packet carrying a comment block and embedded cover art would otherwise set
the ceiling for the whole track.

**Header packets are not columns.** Opus has two and Vorbis three, and the
big ones -- Vorbis's setup packet of codebooks, and OpusTags carrying the
comment block and any embedded picture -- are several KB each. They
clamped to 255, which is why every Ogg drew a full-height spike in its
first column. The codec is identified before its first page's segments
are walked, precisely so the header count is known by the time the first
packet ends.

**The last packet is the end of the stream, not the end of the music.**
Opus pads its final packet and both codecs can finish short; either way
the size says nothing about the audio and it landed in the last column as
a spike. Held at the previous value rather than dropped, so the envelope
still spans the full width.

Verified against a generated 5000-packet Opus stream: 100 s, exact.

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

### The walk declines formats it cannot parse

`framewalk_supports()` reads four bytes before the scan task commits to
anything. Without it, an Ogg was read end to end -- a whole file off the
card, in contention with the decoder reading the same card for the same
track -- to produce zero frames and a log line saying so.

Ogg is walked now -- see below. AAC, TS and AMR remain: those state a
frame length and nothing about the content, so they produce a duration and
no envelope.

### The seek bubble reads as a time

Volume shows a percentage, seek shows MM:SS -- a seek bubble reading "46"
is a unitless number on a bar whose two ends are already clocks. It needs
`len_sec`, so on a format with no duration it falls back to the
percentage.

### The finger bubble tracks x only

Following the finger vertically as well put the bubble at a different
height depending on where in the padded hit box the press landed, which
reads as the indicator jumping rather than as a value changing. It now
sits at a fixed height measured from the seek bar, so both sliders raise
it to the same place.

Erasing it needs `ui_capture_background()`. The bar is cleared wholesale
every frame, but the bubble deliberately reaches above the bar onto the
cover art, which is not -- so a strip of the artwork is saved once after
the cover is drawn and memcpy'd back before each repaint. Without that a
drag leaves a trail of bubbles across the cover.

### Seek

`decoder_seek_sec()` is MP3-only. `MP3D_SEEK_TO_SAMPLE` built a
sample-accurate index at open time, so `mp3dec_ex_seek()` lands exactly
rather than guessing a byte offset. The esp_audio_codec simple decoder has
no seek entry point -- its parsers are forward-only over a stream -- so
FLAC and WAV return `ESP_ERR_NOT_SUPPORTED` and the drag is ignored with a
log line rather than treated as a failure.

Two things happen on a successful seek that are easy to leave out:

- **The ring is reset.** Otherwise ~0.37 s of the old position plays out
  after the jump, which sounds like the seek was ignored and then took
  effect late.
- **`frames_out` is re-anchored.** It is the source of the elapsed clock,
  so without this the time counts on from where it was instead of from the
  new point.

`mp3dec_ex_seek()` counts in int16 values across all channels, the same
units as `ex.samples`. Seeking to `sec * hz` lands at half the intended
point on stereo.

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

### The USB port is off until there is a reason

VBUS is not brought up at boot. Neither is the host stack -- powering the
port but leaving the stack uninstalled would spin a drive up and then
ignore it, which is the worst of both. `storage_usb_enable()` does the
whole sequence: host stack, class driver, then bus power, in that order,
so a drive already in the port is enumerated by a stack that exists.

Two things ask for it, and nothing else does:

1. **No card at boot.** A player with nothing to play should look at the
   other port rather than sit there empty. Boot only, deliberately -- a
   card pulled later is a removal, not a search for media, and tying it to
   removal would mean a card reseated twice had powered the port
   permanently as a side effect. "Card present but unreadable" counts as
   no card, because nothing mounted either way.
2. **The USB tab is selected**, which is the user saying the same thing by
   hand.

It is one-way. Cutting VBUS again would yank a mounted drive out from
under whatever is reading it, and the saving is a port with nothing
plugged into it -- which is the state it was already in.

The work happens on the poll task, not on the caller's: three I2C writes
and a 100 ms settle for the port's inrush, and the two callers are the
boot path and a touch event. Neither should block on it.

### A greyed tab is still a button

This is the one place the chooser departs from a normal tab strip, and it
follows from rule 2: if the port is only powered by selecting its tab,
then the tab has to be selectable while there is nothing behind it. Grey
means "nothing here yet", not "not a button".

So the underline is drawn for the selected tab whether or not anything is
mounted -- in grey rather than red when it is empty. A strip with no
underline at all reads as a lost tap.

The path row carries the reason instead of a path:

| State | Row reads |
| --- | --- |
| No card | `no card in the slot` |
| USB tab, port dark | `tap again to power the USB port` |
| USB tab, port up, no drive | `USB port on - waiting for a drive` |

`storage_usb_powered()` deliberately reports the *request*, not the
completed bring-up. The poll task can be a second behind the tap, and for
that second the row would otherwise tell the user to tap again -- which
either does nothing or reads as the first tap having missed. For the same
reason the request flag is cleared only on failure, never on the way in:
clearing it first leaves a window where neither flag is set and the label
flickers back mid-bring-up.

One consequence worth stating: **the chooser no longer moves the tab on
its own.** It used to fall back to whatever else was mounted when the
shown volume vanished. That would have undone the tap that powered the
port, about a second before the drive it was waiting for turned up.

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
  the same reasoning as resetting the ring on a seek. A track that ended
  on its own still drains, because those fractions of a second are the end
  of the song.
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
has a 20 ms period. It is checked ahead of the pause wait and again in the
idle path, or a chooser dismissed while paused -- or with nothing playing
at all -- leaves its listing on screen.

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
cover out of the tag and decode it there. The ring is 64 KB -- 0.37 s of
44.1 kHz stereo -- and a 3000x3000 cover took 550 ms in the hardware
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

- **Seek on non-MP3 does nothing**, per above.
- **The marquee is pixel-stepped, not eased.** It starts and stops at full
  speed. Easing needs a curve and a frame counter for a 3 px/frame slide,
  which is more state than the effect is worth.
- **Non-ASCII still shows as `?`**, marquee or not. A long title in a
  script `font8x8` does not cover bounces just as legibly and says just as
  little.

### Licensing, since you already care about this for exFAT

- minimp3 is CC0/public domain. No attribution obligation, vendored
  anyway so the source is auditable in-tree.
- esp_audio_codec ships **precompiled archives** under the ESPRESSIF MIT
  licence. Free, but the grant is limited to Espressif silicon. Fine
  here; worth knowing before this code gets copied somewhere it is not.
- pngle and miniz are MIT.

### Open, matching the TODO list above

- **Cover art is ID3v2-only.** `albumart_extract()` finds nothing in a
  FLAC (`PICTURE` metadata block) or an M4A (`covr` atom). Separate
  parser each; not written.
- **Screen sleep** from the TODO list is still just the backlight and the
  moon button; the panel and the decoder stay up.
- **exFAT is still a script, not a default.** Both volumes report the same
  "no mountable filesystem" and point at `tools/enable_exfat.sh`.
- **`.m4a` is a container.** AAC and ALAC inside it work; Apple's
  protected AAC opens and then fails on the first frame.
- **Nothing is peak-limited.** The `peaking?` item is still open — note
  that FLAC and WAV arrive at full scale where MP3 rarely did, so
  whatever the ES8388 output stage does on clipping is now easier to
  reach.

### The ffmpeg vs python-script item

With this patch the transcode advice narrows a lot. `.mp1`/`.mp2`
mislabelled `.mp3` decode natively, and Opus arrives through the Ogg
parser, so the cases that genuinely need re-encoding are ALAC, Vorbis and
anything DRM'd. The first two are only a framing layer away rather than a
codec away. Everything else is a decoder problem, and decoder problems
get fixed in `decoder.c`.
