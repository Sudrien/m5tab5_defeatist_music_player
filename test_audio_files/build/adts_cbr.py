#!/usr/bin/env python3
"""
adts_cbr.py -- turn a variable-rate ADTS file into a constant-rate one.

WHY THIS EXISTS

`cbrseek.c` has an ADTS branch that maps time to offset when the frame
length is provably constant, and nothing in the test folder ever reached
it. ffmpeg's native AAC encoder writes `buffer_fullness = 0x7FF` in
every header -- the stream declaring itself variable -- and cbrseek
believes it and stops there. That refusal is file 08's whole point, so
08 cannot also be the file that exercises the branch.

There is no CBR AAC encoder in a stock ffmpeg build, so the file is
made rather than encoded:

  - every frame is zero-padded to the longest frame in the file, and
    `aac_frame_length` is rewritten to match. The padding sits after
    the raw data block's ID_END terminator, where a decoder that uses
    the length field to find the next frame will step over it.
  - `adts_buffer_fullness` is rewritten from 0x7FF to a fixed value, so
    the stream stops declaring itself variable.

The result is a real, decodable AAC stream whose byte rate genuinely is
a straight line -- which is the property under test. It is not what a
CBR encoder would emit (a real one spends the slack on fill elements
and its bit reservoir); it is a file that has the one property
cbrseek.c looks for, made the only way this toolchain can make one.

Usage: adts_cbr.py in.aac out.aac
"""
import sys

FULLNESS = 0x0FF        # anything but 0x7FF; "not declaring myself VBR"


def frames(d):
    p = 0
    while p + 7 <= len(d):
        if d[p] != 0xFF or (d[p + 1] & 0xF6) != 0xF0:
            raise SystemExit("not an ADTS frame header at %d" % p)
        n = ((d[p + 3] & 0x03) << 11) | (d[p + 4] << 3) | (d[p + 5] >> 5)
        if n < 7 or p + n > len(d):
            break
        yield p, n
        p += n


def main(src, dst):
    d = open(src, "rb").read()
    lens = [n for _, n in frames(d)]
    if not lens:
        raise SystemExit("no frames found")
    target = max(lens)

    out = bytearray()
    for p, n in frames(d):
        h = bytearray(d[p:p + n]) + bytes(target - n)
        # aac_frame_length: 13 bits across bytes 3, 4, 5
        h[3] = (h[3] & 0xFC) | ((target >> 11) & 0x03)
        h[4] = (target >> 3) & 0xFF
        h[5] = (h[5] & 0x1F) | ((target & 0x07) << 5)
        # adts_buffer_fullness: 11 bits across bytes 5, 6
        h[5] = (h[5] & 0xE0) | ((FULLNESS >> 6) & 0x1F)
        h[6] = (h[6] & 0x03) | ((FULLNESS & 0x3F) << 2)
        out += h

    open(dst, "wb").write(bytes(out))
    print("%s: %d frames, %d..%d B -> %d B each, %d B total"
          % (dst, len(lens), min(lens), max(lens), target, len(out)))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__.strip().splitlines()[-1])
    main(sys.argv[1], sys.argv[2])
