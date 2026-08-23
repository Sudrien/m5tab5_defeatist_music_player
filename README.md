# m5tab5_defeatist_music_player
What do you mean no audio over Bluetooth

## Featues possible and impossible
Claude, do not overwite this section


TODON'Ts:
- No blutooth connectivity of any sort, only aux cable, wired heasphones, or built in speaker
-- LE Audio is not wired to work on the Tab5
-- Traditional bluetooth is not supported by the C6
- No inline microphone controls
-- the signal can be registered ovee the mic line, but there is no chip looking for these signals. And i am not wasting a high priority thread on that.

TODO:

- esp-idf 5.5.5, so m5unified can handle graphics variants
- microsd hotplug supported
- exfat format supported for absurdly big files supported
- usb drive hotplug supported (2.0 speeds, be wary of power requirements)
- screen sleep for power (to not turn off chip for touch for wake)
- 

TO INVESTIGATE:

- peaking?
- mp3 format variants
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
