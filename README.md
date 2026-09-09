# M5Tab5 Defeatist Music Player
What do you mean no audio over Bluetooth

Claude, do not touch this README unless explicitly asked to. Use your own file.

![](screenshots/IMG_20260825_212600_264a.jpg)

( [Bôa has a bandcamp by the way](https://boa-uk.bandcamp.com/). You want this because you want artist to actually get paid for their work, right? Not some streaming cents. )


## The M5Tab5 is not an ideal music player.

- You think it has bluetooth.
  - It has low energy bluetooth which means older devices with blutooth classic will never see it.
  - LE Audio / Auracast requires different wiring and profiles. Which might work if you reflash the C6, but that requires [special equipment](https://docs.m5stack.com/en/guide/restore_factory/m5tab5_c6_wifi).
- It has a headset port
  - Which is great for a headset. Or an AUX cable. Or theoretically, recording.
  - But there is nothing listening for inline controls. These can be wired, supposedly. Which, again, means special (but not too special) equipment. 
- The display and touch are controlled by the same chip. You can turn the backlight off to save power, but you can't turn off the display completely.
- The whole display driver mess.
  - Initial release (2025.5.9): separate ILI9881C display driver + GT911 touch controller
  - 2025.10.14: switched to ST7123 display-touch integrated (TDDI) driver
  - 2026.4.28: driver IC changed from ST7123 to ST7121 (this is what I was sent)
  

## Here is what I was able to get working on ESP-IDF 5.5.5

- MicroSD card and USB stick hotplug
  - The microsd card is preferred. It will use less power.
  - It will only auto-mount usb if no microsd is readable
- exFAT support
  - SDHC & SDXC cards have been tested (even if the latter died after week, not the software's fault). SDUC has not. Will Blu-ray size audio files play? Hell if I know.
- Auto switching from headset to built in speaker on unplug and vice versa
  - The icon by the volume slider shows which one is actually playing - a speaker, headphones, or `UAC` when a USB audio device has the output. Tapping mutes and unmutes. 
- Support for all (as far as I can tell) mp3 formats. This thing has fallback library after fallback library. Flac, ogg, wav, the standards are in here.
- Album art display
- Battery status (supposedly)
- Volume control
- play/pause
- start of track/previous
- next track
- screen sleep
- drag to seek. Every format in the list above.
- play order button cycles ONE / ALL / RND (current folder) / RPT (single file)
- volume waveform on the seek bar. I thought it was cool.
- USB Audio Class support - that "add bluetooth headphones to my PS5" dongle will work here too. USB A port only. 
- pause cuts power to the amp
- some sdram caching. If you notice things acting up 20 seconds before a song change, please file an issue.
- ReplayGain support. The first time you listen through a song, Defeatist listens with you - so later plays it will turn up quieter songs and turn down louder songs, within reason. [BS.1770](https://www.itu.int/rec/R-REC-BS.1770/en) reason.
  - The seek bar waveform comes from the same listen. Until a song has been heard all the way through once, its bar is plain grey.
  - Skipping or seeking during that first listen cancels it - it will try again next time.
  - This metadata is in a hidden `.songname.rgcache`. You can't turn off calculation, you can disable volume adjustments in settings.
- ARK 12 covers a good section of unicode, but is not perfect. 
- Reopen last played song on start. Not autoplay.
- 3 second fade on media pull
- configurable crossfade
- actually paying attention to gapless playback data

## v0.4.0 targets
- Internet Radio. Either of these options is "power tether" territory.
  - https://www.radio-browser.info 
- Sleep timer
- NTP
- captive portal wifi config (multiple routers)

## What could happen
- I think there is nothing in dependencies stopping from using esp-idf 6.1
- build file lists faster
- more crash and burn handling, hey, you can always hook it up to `idf.py monitor` and see what you get.
- Podcast over wifi downloader? Conceivable. Would want chapter support
  - there's so much. So so much. 
- Cue sheets - do people actually rip full albums? I just have seen tracks
- m3u/m3u8 - playlists are significant potential UI

## What could not happen with current published code
- classic BT dongle support
- per file resume
- usb hubs - Can it tell you have plugged one in? yes. Can it use things plugged into them? Probably not. Will one save you if your device requires enough power to brownout the Tab5? Uh. Define save.
- DRM'd files are no-go.
- DSD and APE require too much processing

## Potential issues

- Charging from usb C + inserted battery + display on can lead to what seems like a speaker whine, but is not. It's got too much power, cap'n. Or not enough. You got the wrong amout of power, cap'n. 
- file selection is a little slower than I'd like because selecting the first song under your thumb is not what you want
- Aux cables are not necessarily shielded enough against everything you might have around them. Electromanetics "move your phone further away" applies.

## Licensing

- This code is MIT
- minimp3 is CC0/public domain. No attribution obligation, vendored
  anyway so the source is auditable in-tree.
- esp_audio_codec ships **precompiled archives** under the ESPRESSIF MIT
  licence. Free, but the grant is limited to Espressif silicon. Fine
  here; worth knowing before this code gets copied somewhere it is not.
- pngle and miniz are MIT.
- **TJpgDec is ChaN's, under its own licence**, and arrives as the
  `espressif/esp_jpeg` component. Permissive -- free for personal and
  commercial use, source redistribution allowed -- but the copyright
  notice has to be retained, so it travels with a redistribution the
  same way the font's OFL does. Used only as a fallback, for cover art
  the hardware JPEG decoder cannot allocate for: the P4's decoder has no
  scaler, so a 3000 px cover wants 17 MB of PSRAM in one block and does
  not get it.
- MurmurHash2, for cover identity, is public domain. No obligation.
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
- MurmurHash2 is public domain.

## One last insult

- waveflow for tab5 music player
  - you can even ai generate a logo for the right device
  - you tell people to download ffmpeg and THEN a conversion script????????? When they may or may not have python to begin with???? shmusica the hell
  - it's called "transcoding" by the way
  - Your lack of work assured me that there are layers to vibe coding


