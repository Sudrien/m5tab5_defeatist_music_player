import math, struct, wave

SR, DUR = 44100, 60
frames = SR * DUR
L = [0.0]*frames
R = [0.0]*frames

# LEFT: a tone that steps up once a second. Pitch alone tells you the second.
for n in range(frames):
    t = n / SR
    sec = int(t)
    f = 220.0 * (2 ** (sec / 24.0))          # ~2 octaves over the minute
    L[n] = 0.28 * math.sin(2*math.pi*f*t)

# RIGHT: at every 10 s mark, k short beeps for the k-th mark (0 s = 1 beep).
for mark in range(0, DUR, 10):
    k = mark // 10 + 1
    for b in range(k):
        start = int((mark + b*0.18) * SR)
        for n in range(start, min(start + int(0.09*SR), frames)):
            t = (n - start)/SR
            env = math.sin(math.pi * t / 0.09)
            R[n] += 0.5 * env * math.sin(2*math.pi*1400*(n/SR))

# a quiet tick every second in the right channel too, so a 1 s error is audible
for sec in range(DUR):
    start = int(sec*SR)
    for n in range(start, min(start+int(0.012*SR), frames)):
        R[n] += 0.22 * math.sin(2*math.pi*3000*((n-start)/SR))

w = wave.open("landmark.wav", "wb")
w.setnchannels(2); w.setsampwidth(2); w.setframerate(SR)
buf = bytearray()
for n in range(frames):
    l = max(-1.0, min(1.0, L[n])); r = max(-1.0, min(1.0, R[n]))
    buf += struct.pack("<hh", int(l*32000), int(r*32000))
w.writeframes(bytes(buf)); w.close()
print("landmark.wav", frames/SR, "s")
