# m5tab5_defeatist_music_player
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

`ui.c` draws a 150 px transport bar into the panel's scan buffer directly,
the way `albumart.c` already does. No LVGL, no M5Canvas -- five controls
is less code than a toolkit to draw them with.

Played portion of the seek bar and the set portion of volume are both a
thick red bar; the remainder of each is a thin grey line. Dragging either
raises a ring indicator offset above the finger, so the thing being
adjusted is never under the hand.

Volume applies live during a drag, because you want to hear it. Seek
fires once on release -- re-decoding on every poll would thrash the SD
card.

Hit targets are padded well past the drawn shapes (`HIT_PAD_X`,
`HIT_PAD_Y`), and buttons are tested before sliders so a button landing
inside a padded slider box still wins.

With the screen off, a touch only wakes -- it does not also press whatever
was under it, or one tap would turn the screen straight back off.

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
font8x8 at scale 5 (about 3.5 mm, roughly a phone's body text), artist
and album at scale 3, the MM:SS digits 20x38, the slider thumbs 16 px
radius, and the bar itself 276 px.

If the bar is ever resized again, two things are load-bearing rather than
aesthetic:

- `SEEK_X0` has to clear the MM:SS run at both ends. `TIME_W` is derived
  from the digit metrics, so it moves when they do.
- `BUBBLE_ABOVE` has to exceed `SEEK_Y`, or the bubble overlaps the bar.
  That is not cosmetic: the bar is blitted before the bubble is drawn, so
  any part of the bubble inside it gets written to the framebuffer and
  never pushed, and shows stale pixels until the next frame clears them.

### Text rows

Title on its own row, artist and album joined on one line underneath,
both clipped with an ellipsis rather than wrapped. Three stacked rows made
the bar taller than the artwork could spare and artist is the part people
actually read, so album shares a line with it.

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

Elapsed sits left of the seek bar, total right of it, both drawn as seven
segments. No font is linked and vendoring one for two timestamps is not
worth it -- seven segments cover 0-9 and a colon, which is all of MM:SS.

Duration comes from `decoder_duration_sec()`, which only minimp3 can
answer: `MP3D_SEEK_TO_SAMPLE` builds the index up front so `ex.samples` is
known. The esp_audio_codec simple decoder exposes `frame_size`, not stream
length, so FLAC and WAV report 0 -- the total reads `00:00` and the bar
stays empty rather than inventing a scale.

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

### What the controls do not do yet

- **The folder button stops the track** instead of opening a chooser.
  That is the playlist TODO, not a UI one.
- **No filename on screen.** Seven segments do not spell, and a real font
  is the price of that row.
- **Seek on non-MP3 does nothing**, per above.

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
- **Still plays exactly one file and stops.** Hotplug, playlists and
  screen sleep from the TODO list are untouched by this patch.
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
