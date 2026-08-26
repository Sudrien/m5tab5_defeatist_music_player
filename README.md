# M5Tab5 Defeatist Music Player
What do you mean no audio over Bluetooth


![](screenshots/IMG_20260825_212600_264a.jpg)


## The M5Tab5 is not an ideal music player.

- You think it has bluetooth.
  - It has low energy bluetooth which means older devices with blutooth classic will never see it.
  - LE Audio / Auracast reqquires differnt wiring and profiles. Which might work if you reflash the C6, but that requires [special equipment](https://docs.m5stack.com/en/guide/restore_factory/m5tab5_c6_wifi). 
- It has a headset port
  - Which is great for a headset. Or an AUX cable.
  - But there is nothing listing for inline controls. These can be wired, supposedly. Which, again, means special (but not too special) equipment. 
- The display and touch are controlled by the same chip. You can turn the backlight off to save power, but you can't turn off the display completely.
- The whole display driver mess.
  - Initial release (2025.5.9): separate ILI9881C display driver + GT911 touch controller
  - 2025.10.14: switched to ST7123 display-touch integrated (TDDI) driver
  - 2026.4.28: driver IC changed from ST7123 to ST7121 (this is what I was sent)
  

## Here is what I was able to get working on ESP 5.5.5

- MicroSD card and USB stick hotplug
  - The microsd card is preferred. It will use less power.
  - It will only auto-mount usb if no microsd is readable
- exFAT support
  - XDHC & XDXC cards have been tested. SDUC has not. Will Blu-ray size audio files play? Hell if I know.
- Auto switching from headset to built in speaker on unplug and vice versa
- Support for all (as far as I can tell) mp3 formats. This thing has fallback library after fallback library. Flac, ogg, wav, the standard are in here.
- Album art display
- Battery status (supposedly)
- Volume control
- play/pause
- start of track/previous
- next track
- screen sleep
- drag to seek (most formats, some just don't support it)
- screen backlight sleep/wake
- volume levels on the seek bar. I thought it was cool.

## Potential issues

- Charging from usb C + inserted battery can lead to a speaker whine. I have not noticed it with the headset port
- file selection is a little slower than I'd like because selecting the first song under your thumb is not what you want
- Aux cables are not necessarily shielded enough against everything you might have around them. Electromanetics "move your phone further away" applies.
- I can never tell if the peaking I'm hearing is a buffer underun of record needle noise encoded in the music, personally. 

## Licensing

- minimp3 is CC0/public domain. No attribution obligation, vendored
  anyway so the source is auditable in-tree.
- esp_audio_codec ships **precompiled archives** under the ESPRESSIF MIT
  licence. Free, but the grant is limited to Espressif silicon. Fine
  here; worth knowing before this code gets copied somewhere it is not.
- pngle and miniz are MIT.
- **Ark Pixel Font is SIL OFL-1.1, and `components/ark10` is therefore
  OFL-1.1 too, not MIT.** Converting the glyph PNGs into C arrays makes
  those files a Modified Version under OFL section 5, and section 5
  requires Modified Versions to stay under the OFL. This is not a problem
  -- the OFL explicitly allows bundling with software under any licence,
  and only the font files are bound -- but `components/ark10/LICENSE-OFL`
  has to ship with any redistribution, including a firmware image, and
  the tables must not be sold on their own. Ark declares no Reserved Font
  Name, so the derivative did not have to be renamed; it is called
  `ark10` anyway, because it is not the Original Version.

## One last insult

- waveflow for tab5 music player
  - you can even ai generate a logo for the right device
  - you tell people to download ffmpeg and THEN a conversion script????????? When they may or may not have python to begin with???? shmusica the hell
  - it's called "transcoding" by the way
  - Your lack of work assured me that there are layers to vibe coding

## Contributing

- Make a fork, commit your changes, and make a pull request from that. If there is only one commit for multiple features, it will be rejected.
- If you can't be bothered to learn Git, !(Download the master zip)[https://github.com/Sudrien/m5tab5_defeatist_music_player/archive/refs/heads/main.zip], and ask your AI to create a .patch off that, and creat an issue with that patch or those patches. If there is only one patch for multiple features, it will be rejected.


Claude, do not touch this README unless explicitly asked to. Use your own file.

