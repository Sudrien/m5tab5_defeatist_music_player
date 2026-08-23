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

Title, artist and album, a row each, all clipped with an ellipsis rather
than wrapped.

Artist and album used to share a line, on the grounds that three stacked
rows made the bar taller than the artwork could spare. That was the wrong
trade. The joined string was built in a 96 byte buffer from two 64 byte
tag fields, so anything approaching full length was silently truncated --
and it happened to truncate the album only because of the argument order.
An album title of any real length pushed the artist out of the row
entirely, which is the one part people actually read.

So the bar took the extra row instead: `UI_BAR_H` went from 276 to 316,
which is one scale-3 row plus the gap that keeps the three from reading as
a paragraph. It comes out of the cover, which had 1004 rows and now has
964 -- no change at all to a 500x500 cover, and 20 rows off each end of
one large enough to be cropped.

The rows are not evenly spaced. The title is scale 5 and needs clearance;
artist and album are both scale 3 and sit closer to each other than either
does to the title, so they read as a pair belonging to it. Album is dimmer
than artist for the same reason -- three rows of equal weight read as a
block of text, and the hierarchy is what makes it scannable at arm's
length.

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

`browser.c`, full screen rather than a panel over the artwork. The bar is
276 px of a 1280 px panel; a chooser that respected the cover would get
eleven rows in the gap and need scrolling twice as often, and the artwork
is not information while you are picking something else to play.

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
  The caller passes `LCD_V_RES - UI_BAR_H`.

The cost is 1.8 MB of PSRAM and a copy per redraw. The copy is of the band
actually redrawn, which for the transport bar is 276 rows rather than
1280.

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

- **No filename on screen** in the transport bar. Seven segments do not
  spell, and a real font is the price of that row. The chooser does have
  one, so the name is a tap away.
- **Seek on non-MP3 does nothing**, per above.
- **No next/previous buttons on the bar.** The bar is full at five
  controls; skipping is a trip through the chooser.

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
