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

- microsd hotplug supported
- exfat format supported for absurdly big files supported
- usb drive hotplug supported (2.0 speeds, be wary of power requirements)
- screen sleep for power (to not turn off chip for touch for wake)
- 

TO INVESTIGATE:

- peaking?
- mp3 format variants
- why they hell are you telling other people to use a python script. This is an ffmpeg fix if you can't be bothered to pull in libraries
