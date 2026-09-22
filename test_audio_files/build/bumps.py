# bumps.py -- writes bumps.wav, the audio behind 27, and nothing else.
#
# Sixty seconds of a 440 Hz tone whose loudness swells and falls six
# times, once per ten seconds, each swell louder than the one before.
# The landmark is made to be heard; this is made to be SEEN. The seek
# bar's waveform is peak loudness, which the landmark barely moves, so
# every cue track of 22 draws the same beep-then-flat line. Here the
# waveform is the whole content: the file draws six rising bumps, and a
# sheet cutting it 3/2/1 must draw three, two and one -- a track drawing
# six has been measured over the whole image.
#
# SPDX-License-Identifier: MIT
import math, struct, wave

SR, DUR, BUMP = 44100, 60, 10
HEIGHTS = [0.25, 0.38, 0.51, 0.64, 0.77, 0.90]   # bump k, left to right

w = wave.open("bumps.wav", "wb")
w.setnchannels(2); w.setsampwidth(2); w.setframerate(SR)
buf = bytearray()
for n in range(SR * DUR):
    t = n / SR
    k = int(t // BUMP)
    env = math.sin(math.pi * (t - k * BUMP) / BUMP) ** 2   # 0 at each edge
    s = int(32000 * HEIGHTS[k] * env * math.sin(2 * math.pi * 440 * t))
    buf += struct.pack("<hh", s, s)
w.writeframes(bytes(buf)); w.close()
print("bumps.wav", DUR, "s")
